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
#include "../layers/ActivatedRasterOverlay.h"
#include "../providers/RasterOverlayTileProvider.h"
#include "../renderer/IPrepareRendererResources.h"
#include "../renderer/RenderDevice.h"

#include <algorithm>
#include <cstdio>
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
    /// P5b:共享位移模板几何是否活跃(prepare 侧跳过废弃 per-tile VBO 的
    /// 总开关),经 IPrepareRendererResources 查询 Renderer 的 pool 状态,
    /// 与 draw 侧模板 swap 的「pool 非空」门控同源。
    static bool terrainSharedTemplateActive(
        const TilesetContentUploadContext& context) {
        return context.pPrepRenderer != nullptr &&
               context.pPrepRenderer->terrainSharedTemplateActive();
    }
    template <typename LoadRequests,
              typename PrepareUpsampleSourceTileFn,
              typename EnsureTileFn>
    static TileLoadRequestOutcome requestMissingTiles(
        LoadRequests& loadRequests,
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
        std::vector<RasterOverlayProjection> requiredProjections;
        requiredProjections.reserve(context.rasterOverlays.size());
        for (ActivatedRasterOverlay* activeOverlay :
             context.rasterOverlays) {
            if (!activeOverlay || !activeOverlay->visible()) {
                continue;
            }
            RasterOverlayTileProvider* overlayProvider =
                activeOverlay->ensureTileProvider(context.device);
            if (!overlayProvider || !overlayProvider->isReady()) {
                continue;
            }
            const RasterOverlayProjection projection =
                overlayProvider->getProjection();
            if (std::find(
                    requiredProjections.begin(),
                    requiredProjections.end(),
                    projection) == requiredProjections.end()) {
                requiredProjections.push_back(projection);
            }
        }
        return TileMissingRequestScheduler::request(
            loadRequests,
            TileMissingRequestSchedulerInput{
                context.loadLifecycle,
                *budget,
                context.contentProvider,
                context.tiles,
                context.emptyContentRegistry,
                &requiredProjections},
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
                // geomorph(变体 A 父级采样)已停用 —— 待地形连续 LOD 重设计
                // (docs/issues/terrain-continuous-lod-redesign-2026-07-17.md)整体
                // 换成规则栅格 + GPU 高度纹理 + 距离连续 morph。停用原因:变体 A 的
                // morph 起点采到粗祖先,山峰区表现为「从地壳浮上来」;且这里的父级
                // 采样在主线程执行,是拖动卡顿来源。demo 已同步关
                // enableLodTransitionPeriod(shader w=1,无 morph),此处再跳过父采样
                // 彻底消除主线程开销,heightDelta 维持 worker 写入的 0。
                const GltfModel* model =
                    tile.content.renderContent.gltfModelForRead();
                bool hasRenderablePrimitive = false;
                bool hasCompletePrebuiltTerrainPayload =
                    model != nullptr && !model->primitives.empty();
                if (model) {
                    for (const GltfPrimitive& primitive :
                         model->primitives) {
                        if (primitive.vertices.empty() ||
                            primitive.indices.empty()) {
                            continue;
                        }
                        hasRenderablePrimitive = true;
                        const size_t expectedVertexBytes =
                            primitive.vertices.size() *
                            sizeof(TerrainGpuVertex);
                        if (!primitive.hasTerrainWaterMaskMetadata ||
                            !primitive.instances.empty() ||
                            primitive.terrainGpuVertexBytes.size() !=
                                expectedVertexBytes) {
                            hasCompletePrebuiltTerrainPayload = false;
                            break;
                        }
                    }
                }

                // Terrain vertex conversion is already complete in the decode
                // worker. Package that payload through the same CPU-ready
                // builder used by every other upload so render metadata and
                // GPU bytes have one source of truth.
                if (tile.content.renderContent.isTerrainRenderContent() &&
                    hasRenderablePrimitive &&
                    hasCompletePrebuiltTerrainPayload &&
                    context.gpuUploadQueue &&
                    context.device) {
                    std::optional<GpuReadyData> ready =
                        GltfRenderResourcePreparer::prepareCpuWork(
                            tile,
                            context.currentFrameTimeSeconds);
                    if (!ready) {
                        GltfRenderResourcePreparer::prepare(
                            tile,
                            context.device,
                            context.currentFrameTimeSeconds,
                            terrainSharedTemplateActive(context));
                        return;
                    }
                    tile.content.renderContent.asyncGpuUploadPending = true;
                    const TileKey tileKey = tile.key;
                    const std::string cacheKey = TileCacheKey::forTile(tileKey);
                    context.gpuUploadQueue->push(PendingGpuUpload{
                        tileKey,
                        cacheKey,
                        std::move(*ready)});
                } else {
                    // Non-terrain content: synchronous path.
                    // (P5b:原生 heightmap 地形也走此路——无预建 payload;
                    // 判据传入让「必走共享模板」瓦片跳过废弃 VBO 建/传。)
                    GltfRenderResourcePreparer::prepare(
                        tile, context.device,
                        context.currentFrameTimeSeconds,
                        terrainSharedTemplateActive(context));
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
        auto logSlowDrainPhase = [&](const char* scope,
                                     double elapsedMs,
                                     const PendingGpuUpload& pendingUpload) {
            constexpr double kSlowDrainPhaseMs = 1.0;
            if (elapsedMs < kSlowDrainPhaseMs) {
                return;
            }
            char detail[256];
            std::snprintf(
                detail,
                sizeof(detail),
                "cache=%s uploadsThisFrame=%u remaining=%zu",
                pendingUpload.cacheKey.c_str(),
                uploadsThisFrame,
                context.gpuUploadQueue ? context.gpuUploadQueue->size() : 0u);
            platformLog(
                LogLevel::Info,
                "EarthPerf",
                "frame=%llu scope=%s ms=%.3f %s",
                static_cast<unsigned long long>(context.frameNumber),
                scope,
                elapsedMs,
                detail);
        };
        // Cesium Native checks its soft time limit after making at least one
        // main-thread load step. On this renderer, a one-upload floor allowed
        // visible terrain fallback to accumulate during sustained interaction,
        // while the previous four-upload floor produced 20-35 ms bursts.
        // Keep two uploads of interaction progress, then restore the previous
        // catch-up floor once interaction smoothing ends. maxUploadsPerFrame
        // remains the no-budget/backstop limit.
        constexpr uint32_t kInteractionUploadFloor = 2u;
        constexpr uint32_t kIdleUploadFloor = 4u;
        uint32_t minimumUploadsBeforeTimeCheck = kIdleUploadFloor;
        if (budget != nullptr) {
            const FrameResourceBudgetSnapshot snapshot = budget->snapshot();
            if (snapshot.interactionActive || snapshot.smoothingActive) {
                minimumUploadsBeforeTimeCheck = kInteractionUploadFloor;
            }
        }
        while (uploadsThisFrame < maxUploadsPerFrame) {
            if (budget != nullptr &&
                uploadsThisFrame >= minimumUploadsBeforeTimeCheck &&
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
                tile->content.renderContent.asyncGpuUploadPending = false;
                const TileContentUploadCommitAction action =
                    TileContentUploadCommitter::
                        finishRenderResourcePreparation(
                            *tile,
                            false,
                            context.pPrepRenderer);
                if (action.resourcesDirty) {
                    markResourcesDirty(*tile);
                }
                TilePendingUploadCompletion::eraseUpload(
                    context.loadLifecycle,
                    upload->cacheKey);
                processed = true;
                ++uploadsThisFrame;
                continue;
            }

            // Call uploadToGpu — creates GPU buffers, clears
            // asyncGpuUploadPending, and calls markRenderContentDone.
            const double uploadStartMs = perf::nowMs();
            GpuUploadMetrics uploadMetrics;
            bool success = GltfRenderResourcePreparer::uploadToGpu(
                *tile,
                context.device,
                std::move(upload->data),
                &uploadMetrics);
            const double uploadToGpuMs = perf::nowMs() - uploadStartMs;
            logSlowDrainPhase(
                "TilePendingLoad.drainGpuUpload.uploadToGpu",
                uploadToGpuMs,
                *upload);
            if (uploadToGpuMs >= 1.0) {
                platformLog(
                    LogLevel::Info,
                    "EarthPerf",
                    "frame=%llu scope=TilePendingLoad.gpuUploadDetail "
                    "ms=%.3f texture=%.3f vertex=%.3f index=%.3f "
                    "instance=%.3f commit=%.3f bytes=%lld vertexBytes=%lld "
                    "indexBytes=%lld instanceBytes=%lld textureBytes=%lld "
                    "retireBytes=%lld retirePendingBytes=%lld "
                    "retirePendingTasks=%u retireInline=%d "
                    "primitives=%u buffers=%u textures=%u cache=%s",
                    static_cast<unsigned long long>(context.frameNumber),
                    uploadToGpuMs,
                    uploadMetrics.textureUploadMs,
                    uploadMetrics.vertexBufferUploadMs,
                    uploadMetrics.indexBufferUploadMs,
                    uploadMetrics.instanceBufferUploadMs,
                    uploadMetrics.resourceCommitMs,
                    static_cast<long long>(uploadMetrics.totalBytes()),
                    static_cast<long long>(uploadMetrics.vertexBytes),
                    static_cast<long long>(uploadMetrics.indexBytes),
                    static_cast<long long>(uploadMetrics.instanceBytes),
                    static_cast<long long>(uploadMetrics.textureBytes),
                    static_cast<long long>(
                        uploadMetrics.deferredCpuBytes),
                    static_cast<long long>(
                        uploadMetrics.deferredReleasePendingBytes),
                    uploadMetrics.deferredReleasePendingTasks,
                    uploadMetrics.deferredReleaseInlineFallback ? 1 : 0,
                    uploadMetrics.primitiveCount,
                    uploadMetrics.totalBufferCount(),
                    uploadMetrics.textureCount,
                    upload->cacheKey.c_str());
            }

            // Finalize: attach raster overlays, ensure children, etc.
            const double finishStartMs = perf::nowMs();
            const TileContentUploadCommitAction action =
                TileContentUploadCommitter::finishRenderResourcePreparation(
                    *tile,
                    success,
                    context.pPrepRenderer);
            if (action.resourcesDirty) {
                markResourcesDirty(*tile);
            }
            logSlowDrainPhase(
                "TilePendingLoad.drainGpuUpload.finish",
                perf::nowMs() - finishStartMs,
                *upload);
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
