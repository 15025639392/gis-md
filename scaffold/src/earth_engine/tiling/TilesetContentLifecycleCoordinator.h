#pragma once

#include "GltfRenderGeometryBuilder.h"
#include "GltfRenderResourcePreparer.h"
#include "GpuReadyData.h"
#include "GpuUploadQueue.h"
#include "TileCacheKey.h"
#include "TileRenderContentState.h"
#include "TileContentUploadCommitter.h"
#include "TileContentUploadPolicy.h"
#include "TileEmptyContentRegistry.h"
#include "TileFrameBudgetFallback.h"
#include "TileLoadLifecycle.h"
#include "TileLoadTypes.h"
#include "TileMissingRequestScheduler.h"
#include "TilePendingUploadFrameProcessor.h"

#include "../content/GltfContentProvider.h"
#include "../content/GltfModel.h"
#include "../core/async/AsyncSystem.h"
#include "../debug/PerfTimer.h"
#include "../core/resources/FrameResourceBudget.h"
#include "../renderer/RenderDevice.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace earth_engine {

class ActivatedRasterOverlay;
class IPrepareRendererResources;

struct TilesetContentLifecycleContext {
    TileLoadLifecycle& loadLifecycle;
    TilesetContentProvider* contentProvider = nullptr;
    RenderDevice* device = nullptr;
    const std::vector<ActivatedRasterOverlay*>& rasterOverlays;
    const std::unordered_map<std::string, std::unique_ptr<TilesetTile>>& tiles;
    TileEmptyContentRegistry& emptyContentRegistry;
    uint64_t frameNumber = 0;
    uint32_t maximumSimultaneousTileLoads = 20;
    double mainThreadLoadingTimeLimit = 0.0;
    double currentFrameTimeSeconds = 0.0;
    uint32_t smoothedMainThreadUploadLimit = 1;
};

struct TilesetContentUploadContext {
    TileLoadLifecycle& loadLifecycle;
    TilesetContentProvider* contentProvider = nullptr;
    RenderDevice* device = nullptr;
    IPrepareRendererResources* pPrepRenderer = nullptr;
    const std::vector<ActivatedRasterOverlay*>& rasterOverlays;
    TileEmptyContentRegistry& emptyContentRegistry;
    GpuUploadQueue* gpuUploadQueue = nullptr;  // async CPU→GPU pipeline
    uint64_t frameNumber = 0;
    uint32_t maximumSimultaneousTileLoads = 20;
    double mainThreadLoadingTimeLimit = 0.0;
    double currentFrameTimeSeconds = 0.0;
    uint32_t smoothedMainThreadUploadLimit = 1;
};

class TilesetContentLifecycleCoordinator {
public:
    template <typename PrepareUpsampleSourceTileFn, typename EnsureTileFn>
    static TileLoadRequestOutcome requestMissingTiles(
        const std::vector<TileLoadRequest>& loadRequests,
        TilesetContentLifecycleContext context,
        FrameResourceBudget* budget,
        PrepareUpsampleSourceTileFn&& prepareUpsampleSourceTile,
        EnsureTileFn&& ensureTile) {
        FrameResourceBudget localBudget;
        if (!budget) {
            const FrameResourceBudgetConfig config =
                TileFrameBudgetFallback::requestConfig(
                    context.maximumSimultaneousTileLoads,
                    context.mainThreadLoadingTimeLimit);
            localBudget.beginFrame(context.frameNumber, config);
            budget = &localBudget;
        }
        return TileMissingRequestScheduler::request(
            loadRequests,
            TileMissingRequestSchedulerInput{
                context.loadLifecycle,
                *budget,
                context.contentProvider,
                context.tiles,
                context.emptyContentRegistry},
            [](const TileKey& key) {
                return TileCacheKey::forTile(key);
            },
            prepareUpsampleSourceTile,
            ensureTile);
    }

    template <typename EnsureTileFn,
              typename EnsureTileChildrenFn,
              typename MarkResourcesDirtyFn>
    static bool processPendingUploads(
        TilesetContentUploadContext context,
        bool interactionActive,
        bool resourceSmoothingActive,
        FrameResourceBudget* budget,
        EnsureTileFn&& ensureTile,
        EnsureTileChildrenFn&& ensureTileChildren,
        MarkResourcesDirtyFn&& markResourcesDirty) {
        FrameResourceBudget localBudget;
        if (!budget) {
            const FrameResourceBudgetConfig config =
                TileFrameBudgetFallback::uploadConfig(
                    context.maximumSimultaneousTileLoads,
                    context.mainThreadLoadingTimeLimit,
                    interactionActive,
                    resourceSmoothingActive,
                    context.smoothedMainThreadUploadLimit);
            localBudget.beginFrame(context.frameNumber, config);
            budget = &localBudget;
        }
        return TilePendingUploadFrameProcessor::process(
            TilePendingUploadFrameProcessorInput{
                context.loadLifecycle,
                *budget,
                context.contentProvider,
                context.device,
                context.pPrepRenderer,
                context.rasterOverlays,
                context.emptyContentRegistry,
                interactionActive,
            resourceSmoothingActive},
            ensureTile,
            ensureTileChildren,
            [&](TilesetTile& tile) {
                // For terrain content, dispatch CPU work to the worker thread
                // and defer GPU upload to drainGpuUploadQueue.  This moves the
                // ~20ms vertex-building CPU work off the main thread.
                // Async GPU pipeline for terrain content decoded from
                // quantized-mesh tiles.  CPU work (buildVertices) runs on
                // a worker thread; GPU upload runs via drainGpuUploadQueue.
                // Async GPU pipeline for terrain content decoded from
                // quantized-mesh tiles.  Vertex data is pre-built in
                // TerrainGpuVertex format during decode (worker thread),
                // so the main thread only needs GPU buffer creation.
                if (tile.content.renderContent.isTerrainRenderContent() &&
                    tile.content.renderContent.hasGltfModel() &&
                    tile.content.renderContent.gltfContent() &&
                    !tile.content.renderContent.gltfContent()->primitives.empty() &&
                    tile.content.renderContent.gltfContent()
                            ->primitives.front()
                            .hasTerrainWaterMaskMetadata &&
                    !tile.content.renderContent.gltfContent()
                            ->primitives.front()
                            .terrainGpuVertexBytes.empty() &&
                    // Pre-built terrain bytes must match the current primitive
                    // geometry. Upsampled tiles carry stale parent bytes whose
                    // vertex count differs from their (clipped) vertices; using
                    // them uploads parent geometry indexed by child indices ->
                    // corrupt triangles. Fall back to the sync prepare() path,
                    // which rebuilds TerrainGpuVertex from prim.vertices.
                    tile.content.renderContent.gltfContent()
                            ->primitives.front()
                            .terrainGpuVertexBytes.size() ==
                        tile.content.renderContent.gltfContent()
                            ->primitives.front()
                            .vertices.size() * sizeof(TerrainGpuVertex) &&
                    context.gpuUploadQueue && context.device) {
                    tile.content.renderContent.asyncGpuUploadPending = true;
                    const TileKey tileKey = tile.key;
                    const std::string cacheKey = TileCacheKey::forTile(tileKey);
                    GpuUploadQueue* gpuQueue = context.gpuUploadQueue;
                    // Extract pre-built vertex bytes directly from the model.
                    // No CPU work needed — format conversion already happened
                    // during decode on the worker thread (cesium-native pattern).
                    const GltfModel* model =
                        tile.content.renderContent.gltfModelForRead();
                    if (model && !model->primitives.empty()) {
                        const GltfPrimitive& prim = model->primitives.front();
                        GpuReadyData ready;
                        GpuReadyPrimitive gpuPrim;
                        gpuPrim.vertexBytes = prim.terrainGpuVertexBytes;
                        gpuPrim.vertexStride = sizeof(TerrainGpuVertex);
                        gpuPrim.vertexCount = prim.vertices.size();
                        gpuPrim.indices = prim.indices;
                        gpuPrim.indexCount = prim.indices.size();
                        gpuPrim.sortCenterEcef =
                            GltfRenderGeometryBuilder::primitiveSortCenterEcef(
                                prim, tile.content.renderContent.gltfTransform());
                        GltfPrimitiveRenderResources& meta = gpuPrim.metadata;
                        meta.useTerrainVertexFormat = true;
                        meta.hasTerrainWaterMaskMetadata =
                            prim.hasTerrainWaterMaskMetadata;
                        meta.terrainOnlyWater = prim.terrainOnlyWater;
                        meta.terrainOnlyLand = prim.terrainOnlyLand;
                        ready.primitives.push_back(std::move(gpuPrim));
                        gpuQueue->push(PendingGpuUpload{
                            tileKey, cacheKey, std::move(ready)});
                    }
                } else {
                    // Non-terrain content: synchronous path.
                    GltfRenderResourcePreparer::prepare(
                        tile, context.device,
                        context.currentFrameTimeSeconds);
                }
            },
            markResourcesDirty);
    }

    /// Drain the GpuUploadQueue: consume CPU-prepared data and perform
    /// GPU upload + finalize render resources on the main thread.
    /// Called once per frame AFTER processPendingUploads.
    /// Returns true if at least one upload was processed.
    template <typename EnsureTileFn, typename MarkResourcesDirtyFn>
    static bool drainGpuUploadQueue(
        TilesetContentUploadContext context,
        FrameResourceBudget* budget,
        uint32_t maxUploadsPerFrame,
        EnsureTileFn&& ensureTile,
        MarkResourcesDirtyFn&& markResourcesDirty) {
        if (!context.gpuUploadQueue || context.gpuUploadQueue->size() == 0) {
            return false;
        }
        bool processed = false;
        uint32_t uploadsThisFrame = 0;
        // 地形 GPU 上传/finalize 受共享主线程时间预算约束（budget->mainThreadTimeMs，
        // 默认 8ms）：先无条件保底上传 kMinTerrainUploadsPerFrame（= 旧硬编码值，
        // 防饿死 + 零回归），其后按当帧剩余主线程时间继续摊入——便宜瓦片一帧可上多
        // 张，昂贵瓦片自限于时间；maxUploadsPerFrame 为失控上限。这补齐了
        // TilesetOptions.mainThreadLoadingTimeLimit 注释声称、此前对地形 drain 缺失
        // 的墙钟截断（旧实现恒 4/帧且完全脱离预算）。budget==nullptr 时退回纯计数。
        constexpr uint32_t kMinTerrainUploadsPerFrame = 4u;
        while (uploadsThisFrame < maxUploadsPerFrame) {
            if (budget != nullptr &&
                uploadsThisFrame >= kMinTerrainUploadsPerFrame &&
                budget->mainThreadTimeExpired()) {
                break;
            }
            std::optional<PendingGpuUpload> upload =
                context.gpuUploadQueue->tryPop();
            if (!upload) break;

            TilesetTile* tile = ensureTile(upload->tileKey);
            if (!tile) {
                // Tile was unloaded before async GPU work completed.
                TilePendingUploadCompletion::eraseUpload(
                    context.loadLifecycle, upload->cacheKey);
                continue;
            }
            // Safety: tile was reprocessed or unloaded while CPU work ran.
            if (!tile->content.renderContent.asyncGpuUploadPending) {
                TilePendingUploadCompletion::eraseUpload(
                    context.loadLifecycle, upload->cacheKey);
                continue;
            }

            if (!upload->data.valid()) {
                // CPU work failed (no primitives).
                TileContentUploadPolicy::markGltfRenderResourcesFailed(
                    *tile);
                tile->content.renderContent.asyncGpuUploadPending = false;
                continue;
            }

            // Call uploadToGpu — creates GPU buffers, clears
            // asyncGpuUploadPending, and calls markRenderContentDone.
            const double uploadStartMs = perf::nowMs();
            bool success = GltfRenderResourcePreparer::uploadToGpu(
                *tile,
                context.device,
                std::move(upload->data));

            // Finalize: attach raster overlays, ensure children, etc.
            const TileContentUploadCommitAction action =
                TileContentUploadCommitter::finishRenderResourcePreparation(
                    *tile,
                    success,
                    context.pPrepRenderer);
            if (action.resourcesDirty) {
                markResourcesDirty();
            }
            // 计入共享主线程时间预算，供下一次循环的墙钟闸判断（同时让本帧后续
            // 无——terrain drain 是帧内最后的主线程资源工作）。
            if (budget != nullptr) {
                budget->recordElapsed(
                    FrameResourceLane::TerrainFinalize,
                    perf::nowMs() - uploadStartMs);
            }
            // Clear the lifecycle key so the tile can be re-loaded if needed.
            TilePendingUploadCompletion::eraseUpload(
                context.loadLifecycle, upload->cacheKey);
            processed = true;
            ++uploadsThisFrame;
        }
        return processed;
    }
};

} // namespace earth_engine
