#pragma once

#include "../content/GltfTerrainUpsampler.h"
#include "../providers/RasterOverlayTile.h"
#include "../providers/RasterOverlayTileProvider.h"
#include "GltfRenderGeometryBuilder.h"
#include "RasterMappedToTilesetTile.h"
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

    static std::optional<TileLoadResult> createLoadResult(
        TilesetTile& tile) {
        // 口子A 插桩:clip 目前在主线程 TileLoadScheduler 内执行,单次
        // 耗时是否值得 worker 化尚无数据——每次上采样记录一条,真机深
        // 缩放跑一轮即可判断(见审计 P1-10 注记)。
        const double clipStartMs = perf::nowMs();
        std::unique_ptr<GltfModel> childModel = createUpsampledModel(tile);
        const double clipMs = perf::nowMs() - clipStartMs;
        platformLog(LogLevel::Info, "EarthPerf",
            "scope=Tileset.upsampleClip ms=%.3f tile=%d/%d/%d ok=%d",
            clipMs,
            tile.key.z,
            tile.key.x,
            tile.key.y,
            childModel ? 1 : 0);
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

    static std::unique_ptr<GltfModel> createUpsampledModel(
        TilesetTile& tile) {
        if (!tile.content.derivesTerrainFromParent()) {
            return nullptr;
        }

        const TilesetTile* source = findGltfTerrainSource(tile);
        if (!source) {
            return nullptr;
        }

        const GltfModel* parentModel =
            source->content.renderContent.gltfModelForRead();
        if (!parentModel) {
            return nullptr;
        }

        const int textureCoordinateIndex =
            chooseUpsampleTextureCoordinate(*parentModel, *source, tile);
        if (textureCoordinateIndex < 0) {
            return nullptr;
        }
        const bool hasInvertedVCoordinate =
            parentModel->rasterOverlayDetails
                .rasterOverlayInvertedVCoordinates.size() >
                    static_cast<size_t>(textureCoordinateIndex) &&
            parentModel->rasterOverlayDetails
                .rasterOverlayInvertedVCoordinates[
                    static_cast<size_t>(textureCoordinateIndex)];

        const double minimumHeight =
            parentModel->rasterOverlayDetails.boundingRegion.minimumHeight;
        const double maximumHeight =
            parentModel->rasterOverlayDetails.boundingRegion.maximumHeight;
        const Rectangle parentRasterBounds =
            parentModel->rasterOverlayDetails.boundingRegion.rectangle.isEmpty()
            ? source->bounds
            : parentModel->rasterOverlayDetails.boundingRegion.rectangle;
        // Derive the child's details before upsampling: the upsampler
        // renormalizes the kept texcoords to the child window, so the windows
        // and the derived rectangles must come from the same mapping to keep
        // the "UV [0,1] spans rasterOverlayRectangles[set]" invariant.
        RasterOverlayDetails childDetails =
            TileRasterOverlayDetailsDeriver::deriveChildFromParent(
                parentModel->rasterOverlayDetails,
                parentRasterBounds,
                tile.bounds,
                minimumHeight,
                maximumHeight);
        const GltfUpsampleWindows windows = buildUpsampleWindows(
            parentModel->rasterOverlayDetails,
            childDetails,
            parentRasterBounds,
            tile.bounds,
            UpsampledQuadtreeNode{tile.key});

        std::unique_ptr<GltfModel> childModel =
            GltfTerrainUpsampler::upsampleForRasterOverlay(
                *parentModel,
                UpsampledQuadtreeNode{tile.key},
                textureCoordinateIndex,
                hasInvertedVCoordinate,
                &windows);
        if (!childModel) {
            return nullptr;
        }
        childModel->rasterOverlayDetails = std::move(childDetails);
        rebuildTerrainGpuVertexBytes(*childModel);
        return childModel;
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
        return key.schemeId.find("WebMercator") != std::string::npos
            ? RasterOverlayProjection::WebMercator
            : RasterOverlayProjection::Geographic;
    }
};

} // namespace earth_engine
