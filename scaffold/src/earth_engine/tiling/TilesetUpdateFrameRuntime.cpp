#include "TilesetUpdateFrameRuntime.h"

#include "../scene/Camera.h"
#include "TileContentAccess.h"
#include "TileContentLifecycleManager.h"
#include "TileFrameWorkCoordinator.h"
#include "TileRasterOverlayPrefetcher.h"
#include "TileRasterOverlayReadinessPolicy.h"
#include "TileRenderPlanFrameRefresher.h"
#include "TileScheme.h"
#include "TileSelectionRasterOverlayPreparer.h"
#include "Tileset.h"
#include "TilesetProviderDiagnosticsCollector.h"
#include "TilesetSelectionFrameFacade.h"
#include "../debug/PlatformLog.h"

namespace earth_engine {

namespace {

constexpr double kPostInteractionResourceSmoothingSeconds = 1.25;

// 飞行减速段的 cull 豁免。
//
// `TileFrameInteractionTracker` 判 cameraMoving 纯按逐帧位移、不区分驱动源,
// 飞行期每帧位移上千米 ⇒ 恒真 ⇒ cullRequestsWhileMoving 全程延迟请求 ⇒
// **飞到目的地画面是空的**。减速段(进度 > 0.7)关掉它,让目的地瓦片提前进队。
// Cesium 用 `Camera.canPreloadFlight()` 解同一件事。
//
// ⚠️ 为什么改这里而不是把 cameraMoving 翻掉:cameraMoving 还喂
// interactionActive → resourceSmoothingActive,翻它会连带关掉资源平滑
// (一个字段三个消费者)。这里是 cull 这一路的唯一收口。
constexpr double kFlightCullReleaseProgress = 0.7;

bool effectiveCullRequestsWhileMoving(bool configured,
                                      const FrameState& frameState) {
    if (!configured) {
        return false;
    }
    if (frameState.cameraFlightActive &&
        frameState.cameraFlightProgress > kFlightCullReleaseProgress) {
        return false;
    }
    return true;
}

// 根层预载(漏底/黑块根修最后一块,cesium levelZero 语义):钉扎只保证
// "加载过不淘汰",从未加载过的区域(冷会话捏到新经度)整条祖先链无数据,
// finalizer 无祖先可回落 → 选中瓦片被丢 → 高空背景即太空黑(真机 BlackProbe
// darkFrac 51% × HoleQual notex dropz=1-5 逐帧对齐实证)。启动后把全球 z≤2
// 种入加载队列(Preload 组,不与视野竞争),配合钉扎 → "任何区域任何时刻
// 都有可画祖先"从此恒成立,最坏是糊到 z2(1/16 分辨率),不再是黑。
//
// 深度取 2 不取 3:Geographic-TMS 全球 z≤2 = 42 片(GPU 影像 ~11MB 级),
// z≤3 到 170 片 ~43MB —— 地板只要"不黑",z3 的驻留由钉扎负责。
constexpr int kBaseCoveragePreloadMaxZoom = 2;
// 影像小泵每帧限额:预载瓦片不进渲染集,常规 prefetch 泵永远轮不到它们
// ("祖先影像只有进过渲染集才开始拉"的根因),这里单独推进;全就绪后每帧
// 只剩 42 次就绪检查,零成本自愈(overlay 配置变更使就绪位翻假时自动重拉)。
constexpr int kBaseCoverageImageryPumpPerFrame = 4;

} // namespace

void TilesetUpdateFrameRuntime::runBaseCoveragePreload(
    Tileset& tileset,
    const FrameState& frameState,
    IPrepareRendererResources* pPrepRenderer) {
    const TileScheme& scheme = *tileset.tileScheme_;
    if (!tileset.baseCoveragePreloadSeeded_) {
        for (int z = 0; z <= kBaseCoveragePreloadMaxZoom; ++z) {
            for (int x = 0; x < scheme.tileCountX(z); ++x) {
                for (int y = 0; y < scheme.tileCountY(z); ++y) {
                    const TileKey key{scheme.id(), z, x, y};
                    if (tileset.contentAccess_.ensureTile(key)) {
                        tileset.loadQueue_.queue(
                            key, TileLoadPriorityGroup::Preload, 0.0);
                    }
                }
            }
        }
        tileset.baseCoveragePreloadSeeded_ = true;
        platformLog(LogLevel::Info, "BaseCoverage",
                    "preload seeded z0..%d", kBaseCoveragePreloadMaxZoom);
    }
    if (tileset.rasterOverlays_.empty()) {
        return;
    }
    const std::vector<size_t> overlayOrder =
        TileSelectionRasterOverlayPreparer::processingOrder(
            tileset.rasterOverlays_);
    int pumped = 0;
    for (int z = 0; z <= kBaseCoveragePreloadMaxZoom; ++z) {
        for (int x = 0; x < scheme.tileCountX(z); ++x) {
            for (int y = 0; y < scheme.tileCountY(z); ++y) {
                TilesetTile* tile = tileset.tileRegistry_.findTile(
                    TileKey{scheme.id(), z, x, y});
                if (!tile || !tile->canPrepareRasterOverlays()) {
                    continue;
                }
                if (TileRasterOverlayReadinessPolicy::
                        requiredBaseImageryDrawableReady(
                            *tile, tileset.rasterOverlays_)) {
                    continue;
                }
                TileRasterOverlayPrefetcher::prefetch(
                    *tile,
                    tileset.rasterOverlays_,
                    overlayOrder,
                    tileset.device_,
                    tileset.options_.maximumScreenSpaceError,
                    tileset.frameResourceBudget_,
                    pPrepRenderer,
                    frameState.frameId);
                if (++pumped >= kBaseCoverageImageryPumpPerFrame) {
                    return;
                }
            }
        }
    }
}

TilesetUpdateFrameRuntimeResult TilesetUpdateFrameRuntime::run(
    Tileset& tileset,
    const FrameState& frameState,
    IPrepareRendererResources* pPrepRenderer) {
    // cesium-native: increment generation each frame so that
    // RenderCommand validator (non-zero check) accepts terrain commands.
    ++tileset.generation_;

    TileFrameWorkResult frameWork = TileFrameWorkCoordinator::run(
        TileFrameWorkInput{
            tileset.tilePlan_,
            tileset.loadQueue_,
            tileset.selectionCounters_,
            tileset.selectionReuseState_,
            tileset.rasterOverlays_,
            tileset.frameResourceBudget_,
            tileset.device_,
            frameState,
            tileset.resourceRevision_,
            tileset.options_.maximumSimultaneousTileLoads,
            TilesetProviderDiagnosticsCollector::collectContentAndRaster(
                tileset.terrainProviders_.contentProvider(),
                tileset.rasterOverlays_)
                .maximumTransportActiveRequests(
                    TileFrameResourceBudgetPlanInput::
                        kDefaultMaximumTransportActiveRequests),
            tileset.options_.mainThreadLoadingTimeLimit,
            kPostInteractionResourceSmoothingSeconds,
            tileset.options_.maximumScreenSpaceError,
            effectiveCullRequestsWhileMoving(tileset.options_
                                                 .cullRequestsWhileMoving,
                                             frameState),
            tileset.options_.cullRequestsWhileMovingMultiplier,
            tileset.options_.enableTerrainFillProxy,
            tileset.options_.terrainFillProxyGridSize,
            tileset.hasTerrainQuadtree(),
            pPrepRenderer,
            // P5b:hold 期间禁 reuse——见 TileSelectionReuseInput::presentationHeld。
            tileset.shouldHoldPresentationFrame()},
        TileFrameWorkState{
            tileset.cameraMoving_,
            tileset.interactionActiveForFrame_,
            tileset.resourceSmoothingActiveForFrame_,
            tileset.lastInteractionActiveTimeSeconds_,
            tileset.lastCameraPosition_,
            tileset.lastCameraDirection_},
        [&tileset, pPrepRenderer](bool uploadInteractionActive,
                                  bool uploadResourceSmoothingActive,
                                  FrameResourceBudget* budget) {
            return tileset.processPendingLoads(
                uploadInteractionActive,
                uploadResourceSmoothingActive,
                pPrepRenderer,
                budget);
        },
        [&tileset]() {
            tileset.markContentResourcesDirty();
        },
        [&tileset]() {
            return tileset.contentLifecycle_.hasPendingWork();
        },
        [&tileset]() {
            TileRenderPlanFrameRefresher::refresh(
                tileset.tilePlan_,
                tileset.contentAccess_,
                tileset.rasterOverlays_,
                TileRenderPlanFrameRefreshOptions{
                    tileset.interactionActiveForFrame_,
                    tileset.resourceSmoothingActiveForFrame_,
                    tileset.options_.maximumScreenSpaceError});
        },
        [&tileset, pPrepRenderer](
            const FrameState& selectionFrameState,
            TileSelectionPerformanceTimings& selectionTimings) {
            TilesetSelectionFrameFacade::selectTiles(
                tileset,
                selectionFrameState,
                &selectionTimings,
                pPrepRenderer);
        },
        [&tileset](const TileKey& key) {
            return tileset.tileRegistry_.findTile(key);
        },
        [&tileset](const TileKey& key) {
            return tileset.contentAccess_.ensureTile(key);
        },
        [&tileset, pPrepRenderer](TilesetTile& tile) {
            tileset.cacheOwnership_.unloadTileContent(tile, pPrepRenderer);
        },
        [&tileset, pPrepRenderer](TilesetTile& tile) {
            tileset.rasterUpsampledChildren_
                .createRasterOverlayUpsampledChildren(
                    tile,
                    pPrepRenderer,
                    tileset.options_.decoupleImageryFromGeometry);
        },
        [&tileset, pPrepRenderer](TileLoadQueue& requests,
                                  FrameResourceBudget* budget) {
            return tileset.requestMissingContent(
                requests,
                budget,
                pPrepRenderer);
        },
        [&tileset](TilesetTile& tile) {
            tileset.resourceInvalidator_.reconcileTileResources(tile);
        });
    if (frameWork.selectionWork.reusedSelection) {
        tileset.retirePreviousSelectionReferencesForReuse();
    }
    // 根层预载(仅承担底图覆盖的 tileset;内容树 pinBaseCoverage 默认关)。
    if (tileset.options_.pinBaseCoverage) {
        runBaseCoveragePreload(tileset, frameState, pPrepRenderer);
    }
    // Drain the async GPU upload queue.  Terrain CPU work dispatched to
    // worker threads by processPendingLoads lands here for GPU upload.
    double gpuUploadDrainMs = 0.0;
    {
        const double t_drain = perf::nowMs();
        tileset.drainGpuUploadQueue(pPrepRenderer);
        gpuUploadDrainMs = perf::nowMs() - t_drain;
        if (gpuUploadDrainMs > 1.0) {
            platformLog(LogLevel::Info, "EarthPerf",
                "drainGpuUpload: %.2f ms", gpuUploadDrainMs);
        }
    }
    if ((frameState.frameId % 120u) == 0u) {
        platformLog(
            LogLevel::Info,
            "EarthPerf",
            "frame=%llu scope=DeferredCpuRelease pendingBytes=%lld "
            "pendingTasks=%u limitBytes=%lld",
            static_cast<unsigned long long>(frameState.frameId),
            static_cast<long long>(
                GltfRenderResourcePreparer::
                    deferredCpuReleasePendingBytes()),
            GltfRenderResourcePreparer::
                deferredCpuReleasePendingTasks(),
            static_cast<long long>(
                GltfRenderResourcePreparer::
                    deferredCpuReleaseLimitBytes()));
    }

    const TileUpdateUploadRunResult& uploadWork = frameWork.uploadWork;
    const TileUpdateSelectionWorkResult& selectionWork =
        frameWork.selectionWork;
    const TilesetProviderDiagnosticsSnapshot rasterDiagnostics =
        TilesetProviderDiagnosticsCollector::collectContentAndRaster(
            tileset.terrainProviders_.contentProvider(),
            tileset.rasterOverlays_);
    return TilesetUpdateFrameRuntimeResult{
        TileUpdateDebugLogInput{
            tileset.tilePlan_.visibleTiles.size(),
            tileset.loadQueue_.size(),
            selectionWork.computeMs,
            selectionWork.selectorTraversalMs,
            selectionWork.selectorRefineMs,
            selectionWork.selectorRenderPlanMs,
            selectionWork.selectorRequestPlanningMs,
            selectionWork.prefetchMs,
            selectionWork.prefetchBaseMs,
            selectionWork.prefetchFillMs,
            selectionWork.prefetchVisibleMs,
            selectionWork.prefetchLoadQueueMs,
            selectionWork.prefetchAdvanceMs,
            selectionWork.prefetchMapMs,
            selectionWork.prefetchEarlyMapMs,
            selectionWork.requestMs,
            uploadWork.terrainUploadMs,
            uploadWork.rasterUploadMs,
            uploadWork.rasterSelectTaskMs,
            uploadWork.rasterUploadTextureMs,
            uploadWork.rasterTileFinalizeMs,
            uploadWork.rasterBookkeepingMs,
            tileset.contentLifecycle_.loadLifecycle()
                .requestState()
                .totalRequestCount(),
            rasterDiagnostics.rasterSourceRequestsInFlight,
            rasterDiagnostics.rasterActiveMappedSourceSets,
            rasterDiagnostics.rasterPendingSourceFallbacks,
            rasterDiagnostics.rasterInFlightSourceTiles,
            rasterDiagnostics.rasterInFlightSourceWaiters,
            rasterDiagnostics.rasterPendingUploads,
            tileset.selectionCounters_,
            selectionWork.reuseMode,
            selectionWork.reuseRejectReason,
            selectionWork.reusedSelection,
            selectionWork.prefetchVisibleTiles,
            selectionWork.prefetchLoadQueueTiles,
            selectionWork.prefetchAdvanceCount,
            selectionWork.prefetchMapCount,
            selectionWork.prefetchEarlyMapCount,
            selectionWork.prefetchVisibleEarlyMapCount,
            selectionWork.prefetchLoadQueueEarlyMapCount,
            selectionWork.prefetchEarlyMapBudgetExhausted,
            uploadWork.rasterUploadsProcessed,
            uploadWork.rasterMappedUploadsProcessed,
            uploadWork.rasterUploadMaxMs,
            uploadWork.rasterUploadMaxWidth,
            uploadWork.rasterUploadMaxHeight,
            frameWork.interactionActive,
            frameWork.resourceSmoothingActive,
            selectionWork.selectorRefineOverlayMs,
            selectionWork.selectorRefineDecisionMs,
            selectionWork.selectorRefineMaterializeMs,
            selectionWork.selectorRefineCommitMs,
            selectionWork.selectorDetailedTimings,
            uploadWork.rasterSourceFallbackMs,
            uploadWork.rasterSourceSnapshotMs,
            uploadWork.rasterSourceIssueMs,
            uploadWork.rasterUploadQueueSelectMs,
            selectionWork.selectorRefineMaterializeCalls,
            selectionWork.selectorRefineMaterializeChanged,
            selectionWork.selectorRefineMaterializeRetry,
            selectionWork.selectorRefineMaterializeFastPath,
            selectionWork.selectorVisitVisibilityMs,
            selectionWork.selectorVisitInputMetricsMs,
            selectionWork.selectorVisitPolicyMs,
            selectionWork.prefetchRenderPlanMs,
            selectionWork.prefetchRenderPlanUpdateMs,
            selectionWork.prefetchRenderPlanActionMs,
            selectionWork.prefetchRenderPlanTiles,
            selectionWork.prefetchRenderPlanAuthoritativeUpdates,
            selectionWork.prefetchRenderPlanStableReuses,
            gpuUploadDrainMs,
            selectionWork.requestOutcome.classifiedContent,
            selectionWork.requestOutcome
                .classifiedTerrainAvailabilityUpsample,
            selectionWork.requestOutcome.classifiedRasterDetailUpsample,
            selectionWork.requestOutcome.issuedContent,
            selectionWork.requestOutcome
                .issuedTerrainAvailabilityUpsample,
            selectionWork.requestOutcome.issuedRasterDetailUpsample,
            selectionWork.requestOutcome
                .skippedUpsampleWorkerCapacity,
            selectionWork.requestOutcome.skippedMotionCull,
            tileset.totalBytesUsed(),
            tileset.contentBytesUsed(),
            tileset.imageryTextureBytesUsed()}};
}

} // namespace earth_engine
