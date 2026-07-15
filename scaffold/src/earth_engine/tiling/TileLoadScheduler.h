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
                    pass.consumed.push_back(requestKey);
                    continue;
                }

                const TileLoadDispatchResult dispatchResult =
                    TileLoadRequestDispatcher::requestUpsampleClip(
                        input.lifecycle.mutex(),
                        input.lifecycle.condition(),
                        input.lifecycle.requestState(),
                        input.lifecycle.pendingLoads(),
                        input.budget,
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
                        [&markTileContentLoading, &requestKey, &pass]() {
                            markTileContentLoading(requestKey);
                            ++pass.outcome.issued;
                        });
                if (shouldStopAfterDispatch(dispatchResult)) {
                    ++pass.outcome.stoppedAtDispatch;
                    retainRemaining(requestIndex);
                    break;
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
                if (input.budget.cullRequestsWhileMoving() && tileState &&
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
                                    .totalRequestCount()),
                            estimatedFanout)) {
                        pass.outcome.blockedByInflight = true;
                        retainRemaining(requestIndex);
                        break;
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
                        },
                        requestOptionsForTile(
                            *input.contentProvider,
                            tileState,
                            input.requiredRasterOverlayProjections));
                if (shouldStopAfterDispatch(dispatchResult)) {
                    ++pass.outcome.stoppedAtDispatch;
                    retainRemaining(requestIndex);
                    break;
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
        return result == TileLoadDispatchResult::Destroying ||
               result == TileLoadDispatchResult::Blocked;
    }
};

} // namespace earth_engine
