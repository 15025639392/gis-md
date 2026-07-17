#pragma once

#include "TileLoadLifecycle.h"
#include "TileLoadQueue.h"
#include "TileLoadRequestDispatcher.h"
#include "TileLoadRequestPlanner.h"
#include "TileLoadPriorityPolicy.h"
#include "TileRetryBackoffPolicy.h"
#include "TilesetTile.h"
#include "../debug/PerfTimer.h"
#include "TileLoadTypes.h"
#include "TileMotionCullPolicy.h"
#include "TileGltfTerrainUpsampledChildMaterializer.h"
#include "../core/resources/FrameResourceBudget.h"
#include "../content/GltfContentProvider.h"

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace earth_engine {

class TerrainProvider;
class TilesetContentProvider;
struct TilesetTile;

struct TileLoadSchedulerInput {
    TileLoadLifecycle& lifecycle;
    FrameResourceBudget& budget;
    TilesetContentProvider* contentProvider = nullptr;
    const std::vector<RasterOverlayProjection>*
        requiredRasterOverlayProjections = nullptr;
};

class TileLoadScheduler {
public:
    template <typename CacheKeyForTileFn,
              typename MakeSnapshotFn,
              typename IsEmptyTileFn,
              typename PrepareUpsampleSourceTileFn,
              typename MarkTileContentLoadingFn>
    static TileLoadRequestOutcome requestMissingTiles(
        const std::vector<TileLoadRequest>& loadRequests,
        TileLoadSchedulerInput input,
        CacheKeyForTileFn&& cacheKeyForTile,
        MakeSnapshotFn&& makeSnapshot,
        IsEmptyTileFn&& isEmptyTile,
        PrepareUpsampleSourceTileFn&& prepareUpsampleSourceTile,
        MarkTileContentLoadingFn&& markTileContentLoading) {
        return processRequests(
                   std::vector<TileLoadRequest>(
                       loadRequests.begin(),
                       loadRequests.end()),
                   input,
                   std::forward<CacheKeyForTileFn>(cacheKeyForTile),
                   std::forward<MakeSnapshotFn>(makeSnapshot),
                   std::forward<IsEmptyTileFn>(isEmptyTile),
                   std::forward<PrepareUpsampleSourceTileFn>(
                       prepareUpsampleSourceTile),
                   std::forward<MarkTileContentLoadingFn>(
                       markTileContentLoading))
            .outcome;
    }

    template <typename CacheKeyForTileFn,
              typename MakeSnapshotFn,
              typename IsEmptyTileFn,
              typename PrepareUpsampleSourceTileFn,
              typename MarkTileContentLoadingFn>
    static TileLoadRequestOutcome requestMissingTiles(
        TileLoadQueue& loadQueue,
        TileLoadSchedulerInput input,
        CacheKeyForTileFn&& cacheKeyForTile,
        MakeSnapshotFn&& makeSnapshot,
        IsEmptyTileFn&& isEmptyTile,
        PrepareUpsampleSourceTileFn&& prepareUpsampleSourceTile,
        MarkTileContentLoadingFn&& markTileContentLoading) {
        RequestPassResult pass = processRequests(
            loadQueue.takeRequests(),
            input,
            std::forward<CacheKeyForTileFn>(cacheKeyForTile),
            std::forward<MakeSnapshotFn>(makeSnapshot),
            std::forward<IsEmptyTileFn>(isEmptyTile),
            std::forward<PrepareUpsampleSourceTileFn>(
                prepareUpsampleSourceTile),
            std::forward<MarkTileContentLoadingFn>(markTileContentLoading));
        // Source preparation can enqueue parent work while this pass runs.
        // If one of those keys was also present in this pass and was consumed,
        // the lifecycle now owns it; remove the callback-side duplicate before
        // merging genuinely retryable requests back into the next batch.
        for (const TileKey& key : pass.consumed) {
            loadQueue.erase(key);
        }
        loadQueue.mergeRequests(std::move(pass.retained));
        return pass.outcome;
    }

private:
    struct RequestPassResult {
        TileLoadRequestOutcome outcome;
        std::vector<TileLoadRequest> retained;
        std::vector<TileKey> consumed;
    };

    template <typename CacheKeyForTileFn,
              typename MakeSnapshotFn,
              typename IsEmptyTileFn,
              typename PrepareUpsampleSourceTileFn,
              typename MarkTileContentLoadingFn>
    static RequestPassResult processRequests(
        std::vector<TileLoadRequest> requests,
        TileLoadSchedulerInput input,
        CacheKeyForTileFn&& cacheKeyForTile,
        MakeSnapshotFn&& makeSnapshot,
        IsEmptyTileFn&& isEmptyTile,
        PrepareUpsampleSourceTileFn&& prepareUpsampleSourceTile,
        MarkTileContentLoadingFn&& markTileContentLoading) {
        RequestPassResult pass;
        TileLoadPriorityPolicy::sortByPriority(requests);

        const auto retainRemaining = [&](size_t first) {
            pass.retained.insert(
                pass.retained.end(),
                requests.begin() + static_cast<std::ptrdiff_t>(first),
                requests.end());
        };

        for (size_t requestIndex = 0;
             requestIndex < requests.size();
             ++requestIndex) {
            const TileLoadRequest& request = requests[requestIndex];
            {
                std::lock_guard<std::mutex> lock(input.lifecycle.mutex());
                if (input.lifecycle.requestState().destroying()) {
                    retainRemaining(requestIndex);
                    break;
                }
            }

            const TileKey requestKey = request.key;
            const std::string cacheKey = cacheKeyForTile(requestKey);
            if (cacheKey.empty()) {
                ++pass.outcome.skippedEmptyCacheKey;
                pass.consumed.push_back(requestKey);
                continue;
            }
            if (input.lifecycle.containsWorkForCacheKey(cacheKey)) {
                // The request/pending lifecycle is now the sole owner.
                ++pass.outcome.skippedAlreadyPending;
                pass.consumed.push_back(requestKey);
                continue;
            }
            if (isEmptyTile(cacheKey)) {
                ++pass.outcome.skippedEmptyTile;
                pass.consumed.push_back(requestKey);
                continue;
            }

            TilesetTile* tileState = nullptr;
            const TileLoadRequestSnapshot snapshot =
                makeSnapshot(requestKey, cacheKey, tileState);
            // 退避门:临时失败瓦片未到重试时刻则跳过,避免每帧重打服务器
            // (项目自有的体验层退避,cesium-native 无此机制)。
            if (snapshot.loadState == TileLoadState::FailedTemporarily &&
                tileState != nullptr &&
                    !TileRetryBackoffPolicy::isRetryDue(
                        tileState->temporaryFailureRetryNotBeforeMs,
                        perf::nowMs())) {
                ++pass.outcome.skippedClassified;
                pass.retained.push_back(request);
                continue;
            }
            const TileLoadRequestKind requestKind =
                TileLoadRequestPlanner::classify(snapshot);
            recordClassified(
                pass.outcome,
                requestKind,
                snapshot.upsampleKind);

            if (requestKind == TileLoadRequestKind::Skip) {
                ++pass.outcome.skippedClassified;
                pass.consumed.push_back(requestKey);
                continue;
            }

            if (requestKind == TileLoadRequestKind::TerrainContentUpsample) {
                if (!tileState ||
                    !prepareUpsampleSourceTile(
                        *tileState,
                        request.priority)) {
                    ++pass.outcome.skippedUpsampleSourceNotReady;
                    pass.retained.push_back(request);
                    continue;
                }

                const bool needsRasterDetailUpsample =
                    tileState->content.isRasterDetailUpsample();
                const bool hasTerrainContentSource =
                    TileGltfTerrainUpsampledChildMaterializer::
                        canCreateLoadResult(*tileState);
                if (needsRasterDetailUpsample &&
                    !hasTerrainContentSource) {
                    ++pass.outcome.skippedUpsampleNoContentSource;
                    pass.consumed.push_back(requestKey);
                    continue;
                }
                if (!hasTerrainContentSource) {
                    ++pass.outcome.skippedUpsampleNoContentSource;
                    pass.consumed.push_back(requestKey);
                    continue;
                }
                if (!TileLoadRequestDispatcher::
                        hasUpsampleClipWorkerCapacity()) {
                    ++pass.outcome.skippedUpsampleWorkerCapacity;
                    pass.retained.push_back(request);
                    continue;
                }

                // clip worker 化:主线程只建输入快照(整拷父 CPU 模型 + 读
                // overlay 映射),把 40-66ms 的裁剪主体派到 worker。快照失败
                // =source 竞态失效,投失败终态(与旧 createLoadResult 返回
                // nullopt 后 fallback Failed 逐一等价)。
                std::optional<TileGltfTerrainUpsampledChildMaterializer::
                                  UpsampleClipInput>
                    clipInput = TileGltfTerrainUpsampledChildMaterializer::
                        buildClipInput(*tileState);
                if (!clipInput) {
                    const TileLoadDispatchResult terminalResult =
                        TileLoadRequestDispatcher::queueUpsampledLoad(
                            input.lifecycle.mutex(),
                            input.lifecycle.requestState(),
                            input.lifecycle.pendingLoads(),
                            requestKey,
                            cacheKey,
                            request.group,
                            request.priority,
                            TileLoadDomain::TerrainContent,
                            TileLoadResult::createTerminal(
                                TileLoadStatus::Failed));
                    if (shouldStopAfterDispatch(terminalResult)) {
                        ++pass.outcome.stoppedAtDispatch;
                        retainRemaining(requestIndex);
                        break;
                    }
                    if (terminalResult == TileLoadDispatchResult::Skipped) {
                        ++pass.outcome.skippedDispatch;
                        pass.consumed.push_back(requestKey);
                        continue;
                    }
                    markTileContentLoading(requestKey);
                    ++pass.outcome.issued;
                    recordIssuedUpsample(
                        pass.outcome,
                        snapshot.upsampleKind);
                    pass.consumed.push_back(requestKey);
                    continue;
                }

                const TileLoadDispatchResult dispatchResult =
                    TileLoadRequestDispatcher::requestUpsampleClip(
                        input.lifecycle.mutex(),
                        input.lifecycle.condition(),
                        input.lifecycle.requestState(),
                        input.lifecycle.pendingLoads(),
                        requestKey,
                        cacheKey,
                        request.group,
                        request.priority,
                        std::move(*clipInput),
                        [](const TileGltfTerrainUpsampledChildMaterializer::
                               UpsampleClipInput& in) -> TileLoadResult {
                            std::optional<TileLoadResult> wrapped =
                                TileGltfTerrainUpsampledChildMaterializer::
                                    wrapUpsampledModel(
                                        TileGltfTerrainUpsampledChildMaterializer::
                                            clipToModel(in));
                            return wrapped
                                ? std::move(*wrapped)
                                : TileLoadResult::createTerminal(
                                      TileLoadStatus::Failed);
                        },
                        [&markTileContentLoading,
                         &requestKey,
                         &pass,
                         upsampleKind = snapshot.upsampleKind]() {
                            markTileContentLoading(requestKey);
                            ++pass.outcome.issued;
                            recordIssuedUpsample(
                                pass.outcome,
                                upsampleKind);
                        });
                if (shouldStopAfterDispatch(dispatchResult)) {
                    ++pass.outcome.stoppedAtDispatch;
                    retainRemaining(requestIndex);
                    break;
                }
                if (dispatchResult ==
                    TileLoadDispatchResult::WorkerCapacityBlocked) {
                    ++pass.outcome.skippedUpsampleWorkerCapacity;
                    pass.retained.push_back(request);
                    continue;
                }
                if (dispatchResult == TileLoadDispatchResult::Skipped) {
                    ++pass.outcome.skippedDispatch;
                }
                pass.consumed.push_back(requestKey);
                continue;
            }

            if (requestKind == TileLoadRequestKind::Content) {
                if (!input.contentProvider) {
                    ++pass.outcome.skippedNoContentProvider;
                    pass.consumed.push_back(requestKey);
                    continue;
                }
                // cesium-js cullRequestsWhileMoving:相机运动过快(相对瓦片尺寸)
                // 时本帧跳过该瓦片的网络请求(回来时多半已划走),下一帧重评估。
                // 只作用于网络 Content(上采样是本地、无"回来"延迟,不剔除)。
                // 默认关 → 忠实 cesium-native,golden 不变。
                //
                // 近景豁免(**有意偏离 cesium**:cesium 纯按 movementRatio 不分
                // 优先级):Urgent 组=当前帧直接被选中渲染的近景可见瓦片,它们
                // 此刻就在屏上、加载永不浪费,运动期继续请求→停手时数据已到,
                // 大幅缩短"停手后逐块补齐"暂态(真机实测 notReady 峰值 83→33、
                // 补齐 ~4s→~2.1s)。只对 Preload/Normal(远/预取,会随运动划走)
                // 保留 cull。代价=运动期近景请求触发的 raster 纹理上传尖刺
                // (拖动帧时长上升),需真机权衡是否再上异步上传消尖刺。
                if (input.budget.cullRequestsWhileMoving() && tileState &&
                    request.group != TileLoadPriorityGroup::Urgent &&
                    TileMotionCullPolicy::shouldDeferForMotion(
                        TileMotionCullPolicy::Input{
                            true,
                            input.budget.cullRequestsWhileMovingMultiplier(),
                            input.budget.cameraPositionDeltaMagnitude(),
                            TileMotionCullPolicy::boundingSphereRadius(
                                *tileState)})) {
                    ++pass.outcome.skippedMotionCull;
                    pass.retained.push_back(request);
                    continue;
                }
                const int estimatedFanout =
                    input.contentProvider->estimatedRequestFanout(requestKey);
                const FrameResourceLane requestLane =
                    input.contentProvider->providesTerrainQuadtree()
                        ? FrameResourceLane::TerrainRequest
                        : FrameResourceLane::ContentRequest;
                {
                    std::lock_guard<std::mutex> lock(input.lifecycle.mutex());
                    if (!input.budget.hasNetworkInflightCapacity(
                        requestLane,
                        static_cast<uint32_t>(
                            input.lifecycle
                                .requestState()
                                .counts()
                                .contentRequests),
                            estimatedFanout)) {
                        pass.outcome.blockedByInflight = true;
                        pass.retained.push_back(request);
                        continue;
                    }
                }
                const TileLoadDispatchResult dispatchResult =
                    TileLoadRequestDispatcher::requestContent(
                        input.lifecycle.mutex(),
                        input.lifecycle.condition(),
                        input.lifecycle.requestState(),
                        input.lifecycle.pendingLoads(),
                        input.budget,
                        *input.contentProvider,
                        requestKey,
                        cacheKey,
                        request.group,
                        request.priority,
                        [&markTileContentLoading, &requestKey, &pass]() {
                            markTileContentLoading(requestKey);
                            ++pass.outcome.issued;
                            ++pass.outcome.issuedContent;
                        },
                        requestOptionsForTile(
                            *input.contentProvider,
                            tileState,
                            input.requiredRasterOverlayProjections));
                if (dispatchResult ==
                    TileLoadDispatchResult::Destroying) {
                    ++pass.outcome.stoppedAtDispatch;
                    retainRemaining(requestIndex);
                    break;
                }
                if (dispatchResult == TileLoadDispatchResult::Blocked) {
                    pass.retained.push_back(request);
                    continue;
                }
                if (dispatchResult == TileLoadDispatchResult::Skipped) {
                    ++pass.outcome.skippedDispatch;
                }
                pass.consumed.push_back(requestKey);
                continue;
            }
        }

        return pass;
    }

    static void recordClassified(
        TileLoadRequestOutcome& outcome,
        TileLoadRequestKind requestKind,
        TileContentUpsampleKind upsampleKind) {
        if (requestKind == TileLoadRequestKind::Content) {
            ++outcome.classifiedContent;
            return;
        }
        if (requestKind != TileLoadRequestKind::TerrainContentUpsample) {
            return;
        }
        switch (upsampleKind) {
            case TileContentUpsampleKind::TerrainAvailability:
                ++outcome.classifiedTerrainAvailabilityUpsample;
                break;
            case TileContentUpsampleKind::RasterDetail:
                ++outcome.classifiedRasterDetailUpsample;
                break;
            case TileContentUpsampleKind::None:
                break;
        }
    }

    static void recordIssuedUpsample(
        TileLoadRequestOutcome& outcome,
        TileContentUpsampleKind upsampleKind) {
        switch (upsampleKind) {
            case TileContentUpsampleKind::TerrainAvailability:
                ++outcome.issuedTerrainAvailabilityUpsample;
                break;
            case TileContentUpsampleKind::RasterDetail:
                ++outcome.issuedRasterDetailUpsample;
                break;
            case TileContentUpsampleKind::None:
                break;
        }
    }

    static TileContentRequestOptions requestOptionsForTile(
        const TilesetContentProvider& provider,
        const TilesetTile* tile,
        const std::vector<RasterOverlayProjection>*
            requiredRasterOverlayProjections) {
        TileContentRequestOptions options;
        if (!provider.providesTerrainQuadtree()) {
            return options;
        }
        if (requiredRasterOverlayProjections) {
            options.requiredRasterOverlayProjections =
                *requiredRasterOverlayProjections;
        }
        if (!tile) {
            return options;
        }
        for (const TilesetTile* child : tile->children) {
            if (child && child->content.isTerrainAvailabilityUpsample()) {
                options.generateTerrainRasterOverlayDetails = true;
                break;
            }
        }
        return options;
    }

    static bool shouldStopAfterDispatch(TileLoadDispatchResult result) {
        return result == TileLoadDispatchResult::Destroying;
    }
};

} // namespace earth_engine
