#pragma once

#include "../content/GltfTerrainUpsampler.h"
#include "../providers/RasterOverlayTile.h"
#include "../providers/RasterOverlayTileProvider.h"
#include "GltfRenderGeometryBuilder.h"
#include "RasterMappedToTilesetTile.h"
#include "TerrainRasterOverlayProjectionResolver.h"
#include "TileRasterOverlayDetailsDeriver.h"
#include "TileLoadTypes.h"
#include "TilesetTile.h"
#include "../debug/PerfTimer.h"
#include "../debug/PlatformLog.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace earth_engine {

class TileGltfTerrainUpsampledChildMaterializer {
public:
    // clip worker 化:主线程 dispatch 前从父/子瓦片拷出的 clip 输入快照。
    // worker 只消费自有快照,**零 TilesetTile/GltfModel 指针**——父瓦片被
    // 主线程 unload(clearRenderContent)或重载(prepareGltfContent)析构都
    // 与 worker 输入解耦,从根上消除跨线程读父模型的 UAF。parentModel 是
    // 父级 CPU 模型的独立整拷(纯 vector 聚合,拷贝=memcpy 级)。
    struct UpsampleClipInput {
        std::unique_ptr<GltfModel> parentModel;
        TileKey childKey;
        Rectangle tileBounds;
        Rectangle sourceBounds;
        int textureCoordinateIndex = -1;
        bool hasInvertedVCoordinate = false;
    };

    static const TilesetTile* findGltfTerrainSource(const TilesetTile& tile) {
        const TilesetTile* parent = tile.parent;
        if (!parent) {
            return nullptr;
        }
        return isCommittedGltfTerrainSource(tile, *parent)
            ? parent
            : nullptr;
    }

    static bool isCommittedGltfTerrainSource(const TilesetTile& tile) {
        return tile.hasCommittedRenderContent() &&
               tile.content.renderContent.isTerrainRenderContent() &&
               tile.content.renderContent.hasGltfContent();
    }

    // 主线程侧:读父瓦片 gltfModel 与 overlay 映射(chooseUpsampleTexture-
    // Coordinate 读 source.rasterOverlayState.mappings() 里的 RasterOverlayTile
    // 有独立生命周期,只能主线程读),把 clip 所需输入整拷进快照。返回
    // nullopt 表示前置不满足(与旧同步路径的早退条件逐一等价)。
    static std::optional<UpsampleClipInput> buildClipInput(TilesetTile& tile) {
        const double startMs = perf::nowMs();
        if (!tile.content.derivesTerrainFromParent()) {
            return std::nullopt;
        }
        const TilesetTile* source = findGltfTerrainSource(tile);
        if (!source) {
            return std::nullopt;
        }
        const GltfModel* parentModel =
            source->content.renderContent.gltfModelForRead();
        if (!parentModel) {
            return std::nullopt;
        }
        const int textureCoordinateIndex =
            chooseUpsampleTextureCoordinate(*parentModel, *source, tile);
        if (textureCoordinateIndex < 0) {
            return std::nullopt;
        }
        const bool hasInvertedVCoordinate =
            parentModel->rasterOverlayDetails
                .rasterOverlayInvertedVCoordinates.size() >
                    static_cast<size_t>(textureCoordinateIndex) &&
            parentModel->rasterOverlayDetails
                .rasterOverlayInvertedVCoordinates[
                    static_cast<size_t>(textureCoordinateIndex)];

        UpsampleClipInput input;
        input.parentModel = cloneParentForClip(*parentModel);
        input.childKey = tile.key;
        input.tileBounds = tile.bounds;
        input.sourceBounds = source->bounds;
        input.textureCoordinateIndex = textureCoordinateIndex;
        input.hasInvertedVCoordinate = hasInvertedVCoordinate;
        platformLog(LogLevel::Info, "EarthPerf",
            "scope=Tileset.upsampleClipInput ms=%.3f tile=%d/%d/%d",
            perf::nowMs() - startMs, tile.key.z, tile.key.x, tile.key.y);
        return input;
    }

    // worker 侧:从快照跑裁剪主体(40-66ms 的重活)。只读 input,不碰任何
    // TilesetTile 或共享 GltfModel。契约一(rebuildTerrainGpuVertexBytes 公
    // 式)与契约二(子 bounds=tileBounds scheme 矩形)公式一字不动。
    static std::unique_ptr<GltfModel> clipToModel(
        const UpsampleClipInput& input) {
        const double clipStartMs = perf::nowMs();
        const GltfModel& parentModel = *input.parentModel;
        const double minimumHeight =
            parentModel.rasterOverlayDetails.boundingRegion.minimumHeight;
        const double maximumHeight =
            parentModel.rasterOverlayDetails.boundingRegion.maximumHeight;
        const Rectangle parentRasterBounds =
            parentModel.rasterOverlayDetails.boundingRegion.rectangle.isEmpty()
            ? input.sourceBounds
            : parentModel.rasterOverlayDetails.boundingRegion.rectangle;
        RasterOverlayDetails childDetails =
            TileRasterOverlayDetailsDeriver::deriveChildFromParent(
                parentModel.rasterOverlayDetails,
                parentRasterBounds,
                input.tileBounds,
                minimumHeight,
                maximumHeight);
        const GltfUpsampleWindows windows = buildUpsampleWindows(
            parentModel.rasterOverlayDetails,
            childDetails,
            parentRasterBounds,
            input.tileBounds,
            UpsampledQuadtreeNode{input.childKey});
        std::unique_ptr<GltfModel> childModel =
            GltfTerrainUpsampler::upsampleForRasterOverlay(
                parentModel,
                UpsampledQuadtreeNode{input.childKey},
                input.textureCoordinateIndex,
                input.hasInvertedVCoordinate,
                &windows);
        if (childModel) {
            childModel->rasterOverlayDetails = std::move(childDetails);
            rebuildTerrainGpuVertexBytes(*childModel);
        }
        // 口子A 插桩:worker 化后测的是 worker 线程耗时(主线程该测点在
        // buildClipInput 的 upsampleClipInput,只剩快照拷贝成本)。
        platformLog(LogLevel::Info, "EarthPerf",
            "scope=Tileset.upsampleClip ms=%.3f tile=%d/%d/%d ok=%d",
            perf::nowMs() - clipStartMs,
            input.childKey.z, input.childKey.x, input.childKey.y,
            childModel ? 1 : 0);
        return childModel;
    }

    static std::optional<TileLoadResult> wrapUpsampledModel(
        std::unique_ptr<GltfModel> childModel) {
        if (!childModel) {
            return std::nullopt;
        }
        TileLoadResultMetadata metadata;
        const auto& region = childModel->rasterOverlayDetails.boundingRegion;
        metadata.rasterOverlayDetails = childModel->rasterOverlayDetails;
        metadata.terrainHeightRange = {
            region.minimumHeight,
            region.maximumHeight};
        return TileLoadResult::createRenderableGltfTerrain(
            std::move(childModel),
            std::move(metadata));
    }

    // 同步入口(现仅测试与 createUpsampledModel 等价性校验用):经统一的
    // buildClipInput+clipToModel 单一逻辑源,与 worker 化路径产出逐字节一致。
    static std::optional<TileLoadResult> createLoadResult(TilesetTile& tile) {
        return wrapUpsampledModel(createUpsampledModel(tile));
    }

    static bool canCreateLoadResult(const TilesetTile& tile) {
        if (!tile.content.derivesTerrainFromParent()) {
            return false;
        }

        const TilesetTile* source = findGltfTerrainSource(tile);
        if (!source) {
            return false;
        }

        const GltfModel* parentModel =
            source->content.renderContent.gltfModelForRead();
        if (!parentModel) {
            return false;
        }

        return chooseUpsampleTextureCoordinate(*parentModel, *source, tile) >=
               0;
    }

    /// Snapshot copy for the clip worker. The clip path never reads the
    /// six runtime-only CPU payloads below (they used to be pruned right
    /// after a full deep copy — a pure white copy, baseVertices alone is
    /// 104B/vertex on the main thread). Instead of copy-then-prune, they
    /// are moved OUT of the parent for the duration of the copy and moved
    /// back before returning: the copy constructor sees empty vectors, so
    /// the snapshot is born pruned and the copy-ctor semantics still cover
    /// every other (including future) member automatically.
    /// Single-threaded contract: called on the main thread only; no other
    /// thread reads a live model's runtime payloads (workers only read
    /// their own snapshots), and the parent is bit-identical on return —
    /// no renderer state derives from these fields, so no cache
    /// invalidation is warranted (which is why this bypasses
    /// editGltfContent and its markRetainedResourcesChanged side effect).
    static std::unique_ptr<GltfModel> cloneParentForClip(
        const GltfModel& parentModel) {
        struct RuntimePayloadSteal {
            struct Payload {
                std::vector<uint8_t> terrainGpuVertexBytes;
                std::vector<GltfInstance> instances;
                std::vector<SurfaceVertex> baseVertices;
                std::vector<std::array<float, 4>> baseTangents;
                std::vector<GltfVertexSkinning> skinning;
                std::vector<GltfMorphTarget> morphTargets;
            };
            GltfModel& model;
            std::vector<Payload> payloads;

            explicit RuntimePayloadSteal(GltfModel& liveModel)
                : model(liveModel) {
                payloads.resize(model.primitives.size());
                for (size_t i = 0; i < model.primitives.size(); ++i) {
                    GltfPrimitive& primitive = model.primitives[i];
                    Payload& payload = payloads[i];
                    payload.terrainGpuVertexBytes =
                        std::move(primitive.terrainGpuVertexBytes);
                    payload.instances = std::move(primitive.instances);
                    payload.baseVertices =
                        std::move(primitive.runtime.baseVertices);
                    payload.baseTangents =
                        std::move(primitive.runtime.baseTangents);
                    payload.skinning =
                        std::move(primitive.runtime.skinning);
                    payload.morphTargets =
                        std::move(primitive.runtime.morphTargets);
                }
            }
            // Restore runs on every exit path (including a throwing copy).
            ~RuntimePayloadSteal() {
                for (size_t i = 0; i < payloads.size() &&
                                   i < model.primitives.size();
                     ++i) {
                    GltfPrimitive& primitive = model.primitives[i];
                    Payload& payload = payloads[i];
                    primitive.terrainGpuVertexBytes =
                        std::move(payload.terrainGpuVertexBytes);
                    primitive.instances = std::move(payload.instances);
                    primitive.runtime.baseVertices =
                        std::move(payload.baseVertices);
                    primitive.runtime.baseTangents =
                        std::move(payload.baseTangents);
                    primitive.runtime.skinning =
                        std::move(payload.skinning);
                    primitive.runtime.morphTargets =
                        std::move(payload.morphTargets);
                }
            }
        };

        // The steal is a scoped, exactly-restored mutation of main-thread
        // owned state; the const view is the tile API convention, not an
        // immutability guarantee.
        GltfModel& mutableParent = const_cast<GltfModel&>(parentModel);
        RuntimePayloadSteal steal(mutableParent);
        return std::make_unique<GltfModel>(mutableParent);
    }

private:

    static bool isCommittedGltfTerrainSource(
        const TilesetTile& child,
        const TilesetTile& source) {
        const bool sourceStateReady =
            child.content.isRasterDetailUpsample()
                ? source.hasCommittedRenderContent()
                : (source.content.loadState == TileLoadState::Done ||
                   source.content.loadState == TileLoadState::Unloading);
        return sourceStateReady &&
               source.content.renderContent.isTerrainRenderContent() &&
               source.content.renderContent.hasGltfContent();
    }

    // 同步 clip:经 buildClipInput(主线程读+快照)→ clipToModel(裁剪)单一
    // 逻辑源。worker 化路径复用同一对函数,保证两路产出逐字节一致。
    // Derive-before-upsample 的不变量("UV [0,1] spans rasterOverlay-
    // Rectangles[set]",窗口与派生矩形须同源)由 clipToModel 内维持。
    static std::unique_ptr<GltfModel> createUpsampledModel(
        TilesetTile& tile) {
        std::optional<UpsampleClipInput> input = buildClipInput(tile);
        if (!input) {
            return nullptr;
        }
        return clipToModel(*input);
    }

    /// P1-10:clip 后为子瓦片重建 TerrainGpuVertex 预置字节。子 primitive
    /// 从父级拷贝来的字节与 clip 后顶点数失配,上传编排的尺寸校验会把该
    /// 瓦片打回主线程同步 prepare(顶点全量重建 + 无预算 GPU 上传,深缩放
    /// 掉帧尖峰来源)。在此按 QuantizedMeshContentLoader 的 decode 后预建
    /// 同一公式重建(顶点已是 ECEF 世界坐标、identity transform、origin 用
    /// 模型首选局部原点,与运行时 renderLocalOrigin 同源),让上采样瓦片
    /// 自然走 GpuUploadQueue 异步路。重建失败则清空字节回落同步路径——
    /// 也顺带消除"父级残留字节尺寸巧合匹配 → 上传父几何"的既有隐患。
    static void rebuildTerrainGpuVertexBytes(GltfModel& childModel) {
        for (GltfPrimitive& primitive : childModel.primitives) {
            primitive.terrainGpuVertexBytes.clear();
            if (!childModel.preferredLocalOriginEcef ||
                primitive.vertices.empty()) {
                continue;
            }
            std::vector<TerrainGpuVertex> terrainVerts =
                GltfRenderGeometryBuilder::buildTerrainVertices(
                    primitive,
                    Mat4::identity(),
                    *childModel.preferredLocalOriginEcef);
            if (terrainVerts.size() != primitive.vertices.size()) {
                continue;
            }
            primitive.terrainGpuVertexBytes.resize(
                terrainVerts.size() * sizeof(TerrainGpuVertex));
            std::memcpy(
                primitive.terrainGpuVertexBytes.data(),
                terrainVerts.data(),
                primitive.terrainGpuVertexBytes.size());
        }
    }

    // Child window inside the parent's UV space for one texcoord set: the
    // relative position of the derived child rectangle within the parent's
    // rectangle, both in that set's projected coordinates. Falls back to the
    // exact half-split quadrant when rectangles are unusable.
    static GltfUpsampleUvWindow uvWindowFromRects(
        const Rectangle& parentRect,
        const Rectangle& childRect,
        RasterOverlayProjection projection,
        bool invertedV,
        const GltfUpsampleUvWindow& fallback) {
        if (parentRect.isEmpty() || childRect.isEmpty()) {
            return fallback;
        }
        const double width = parentRect.width();
        const double height = parentRect.height();
        if (!(width > 0.0) || !(height > 0.0)) {
            return fallback;
        }
        const bool crosses =
            projection == RasterOverlayProjection::Geographic &&
            parentRect.crossesAntimeridian();
        const auto relativeX = [&](double x) {
            double offset = x - parentRect.west();
            if (crosses && offset < 0.0) {
                offset += MathUtils::TwoPi;
            }
            return offset / width;
        };
        GltfUpsampleUvWindow window;
        window.u0 = relativeX(childRect.west());
        window.u1 = relativeX(childRect.east());
        // NW 约定（invertedV=false）：v=0 在北；inverted 时翻成 south-based
        double v0 = (parentRect.north() - childRect.north()) / height;
        double v1 = (parentRect.north() - childRect.south()) / height;
        if (invertedV) {
            const double flipped = 1.0 - v1;
            v1 = 1.0 - v0;
            v0 = flipped;
        }
        window.v0 = v0;
        window.v1 = v1;
        if (!(window.u1 > window.u0) || !(window.v1 > window.v0)) {
            return fallback;
        }
        return window;
    }

    static GltfUpsampleWindows buildUpsampleWindows(
        const RasterOverlayDetails& parentDetails,
        const RasterOverlayDetails& childDetails,
        const Rectangle& parentGeographicBounds,
        const Rectangle& childGeographicBounds,
        const UpsampledQuadtreeNode& childID) {
        GltfUpsampleWindows windows;
        windows.texCoordSetCount = std::min(
            parentDetails.rasterOverlayProjections.size(),
            kGltfMaxTexCoordSets);
        for (size_t i = 0; i < windows.texCoordSetCount; ++i) {
            const bool invertedV =
                i < parentDetails.rasterOverlayInvertedVCoordinates.size() &&
                parentDetails.rasterOverlayInvertedVCoordinates[i];
            const Rectangle parentRect =
                i < parentDetails.rasterOverlayRectangles.size()
                    ? parentDetails.rasterOverlayRectangles[i]
                    : Rectangle::EMPTY;
            const Rectangle childRect =
                i < childDetails.rasterOverlayRectangles.size()
                    ? childDetails.rasterOverlayRectangles[i]
                    : Rectangle::EMPTY;
            windows.texCoordSets[i] = uvWindowFromRects(
                parentRect,
                childRect,
                parentDetails.rasterOverlayProjections[i],
                invertedV,
                GltfTerrainUpsampler::quadrantUvWindow(childID, invertedV));
        }
        // 原生 uv 同为 NW 约定（QuantizedMeshParser 解码时已翻转）
        windows.nativeUv = uvWindowFromRects(
            parentGeographicBounds,
            childGeographicBounds,
            RasterOverlayProjection::Geographic,
            false,
            GltfTerrainUpsampler::quadrantUvWindow(childID, false));
        return windows;
    }

    static bool modelHasTextureCoordinate(const GltfModel& model,
                                          int textureCoordinateIndex) {
        if (textureCoordinateIndex < 0 ||
            textureCoordinateIndex >= static_cast<int>(kGltfMaxTexCoordSets)) {
            return false;
        }
        const size_t index = static_cast<size_t>(textureCoordinateIndex);
        for (const GltfPrimitive& primitive : model.primitives) {
            if (!primitive.vertices.empty() &&
                primitive.vertexTexCoords[index].size() ==
                    primitive.vertices.size()) {
                return true;
            }
        }
        return false;
    }

    static int chooseUpsampleTextureCoordinate(const GltfModel& model,
                                               const TilesetTile& source,
                                               const TilesetTile& tile) {
        if (tile.content.isTerrainAvailabilityUpsample()) {
            const int candidate =
                model.rasterOverlayDetails.textureCoordinateIDForProjection(
                    terrainProjectionForTileKey(tile.key));
            return modelHasTextureCoordinate(model, candidate) ? candidate
                                                               : -1;
        }

        if (tile.content.isRasterDetailUpsample()) {
            if (tile.content.rasterDetailSourceProjection) {
                const int candidate =
                    model.rasterOverlayDetails.textureCoordinateIDForProjection(
                        *tile.content.rasterDetailSourceProjection);
                return modelHasTextureCoordinate(model, candidate)
                    ? candidate
                    : -1;
            }

            for (const auto& mapping : source.rasterOverlayState.mappings()) {
                if (!mapping || !mapping->isMoreDetailAvailable()) {
                    continue;
                }
                const RasterOverlayTile* readyTile = mapping->getReadyTile();
                if (!readyTile) {
                    continue;
                }
                const RasterOverlayProjection projection =
                    readyTile->getTileProvider().getProjection();
                const int detailsTextureCoordinate =
                    model.rasterOverlayDetails.textureCoordinateIDForProjection(
                        projection);
                if (modelHasTextureCoordinate(
                        model,
                        detailsTextureCoordinate)) {
                    return detailsTextureCoordinate;
                }
            }
            return -1;
        }

        const int geographic =
            model.rasterOverlayDetails.textureCoordinateIDForProjection(
                RasterOverlayProjection::Geographic);
        if (modelHasTextureCoordinate(model, geographic)) {
            return geographic;
        }

        for (RasterOverlayProjection projection :
             model.rasterOverlayDetails.rasterOverlayProjections) {
            const int candidate =
                model.rasterOverlayDetails.textureCoordinateIDForProjection(
                    projection);
            if (modelHasTextureCoordinate(model, candidate)) {
                return candidate;
            }
        }

        return -1;
    }

    static RasterOverlayProjection terrainProjectionForTileKey(
        const TileKey& key) {
        return TerrainRasterOverlayProjectionResolver::forTileKey(key);
    }
};

} // namespace earth_engine
