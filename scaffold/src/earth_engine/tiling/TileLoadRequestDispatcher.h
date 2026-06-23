#pragma once

#include "TileLoadTypes.h"
#include "TilePendingLoadQueue.h"
#include "TilePendingRequestState.h"
#include "TileLoadPriorityPolicy.h"
#include "../core/resources/FrameResourceBudget.h"
#include "../content/GltfContentProvider.h"
#include "../threading/CancellationToken.h"

#include <condition_variable>
#include <mutex>
#include <string>
#include <utility>

namespace earth_engine {

enum class TileLoadDispatchResult {
    Issued,
    Skipped,
    Blocked,
    Destroying
};

class TileLoadRequestDispatcher {
public:
    static TileLoadDispatchResult queueUpsampledLoad(
        std::mutex& mutex,
        TilePendingRequestState& requestState,
        TilePendingLoadQueue& pendingLoads,
        const TileKey& key,
        const std::string& cacheKey,
        TileLoadPriorityGroup group,
        double priority,
        TileLoadDomain domain,
        TileLoadResult result);

    template <typename OnIssuedFn>
    static TileLoadDispatchResult requestContent(
        std::mutex& mutex,
        std::condition_variable& condition,
        TilePendingRequestState& requestState,
        TilePendingLoadQueue& pendingLoads,
        FrameResourceBudget& budget,
        TilesetContentProvider& provider,
        const TileKey& key,
        const std::string& cacheKey,
        TileLoadPriorityGroup group,
        double priority,
        OnIssuedFn&& onIssued) {
        CancellationToken token;
        const int estimatedFanout = provider.estimatedRequestFanout(key);
        const FrameResourceLane requestLane =
            provider.providesTerrainQuadtree()
                ? FrameResourceLane::TerrainRequest
                : FrameResourceLane::ContentRequest;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (requestState.destroying()) {
                return TileLoadDispatchResult::Destroying;
            }
            if (cacheKey.empty()) {
                return TileLoadDispatchResult::Skipped;
            }
            if (requestState.contains(cacheKey) ||
                pendingLoads.containsCacheKey(cacheKey)) {
                return TileLoadDispatchResult::Skipped;
            }
            if (!budget.tryIssue(
                    requestLane,
                    TileLoadPriorityPolicy::toFramePriority(group),
                    estimatedFanout)) {
                return TileLoadDispatchResult::Blocked;
            }
            if (!requestState.beginContentRequest(cacheKey, token)) {
                return TileLoadDispatchResult::Skipped;
            }
        }

        onIssued();
        const TileLoadDomain domain =
            provider.providesTerrainQuadtree()
                ? TileLoadDomain::TerrainContent
                : TileLoadDomain::Content;
        provider.requestTileContent(
            key,
            token,
            [&mutex,
             &condition,
             &requestState,
             &pendingLoads,
             cacheKey,
             key,
             token,
             group,
             priority,
             domain,
             &provider](const TileKey&, TileContentLoadResult result) mutable {
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (!requestState.destroying() && !token.isCancelled()) {
                        TileLoadResult loadResult =
                            TileLoadResult::fromContentResult(
                                std::move(result));
                        if (loadResult.content.hasGltfTerrainPayload() &&
                            !loadResult.content
                                 .quantizedMeshAvailabilityUpdatesApplied &&
                            !loadResult.content
                                 .quantizedMeshAvailabilityUpdates.empty() &&
                            provider.providesTerrainQuadtree()) {
                            provider.applyTerrainAvailabilityUpdates(
                                loadResult.content
                                    .quantizedMeshAvailabilityUpdates);
                            loadResult.content
                                .quantizedMeshAvailabilityUpdatesApplied =
                                true;
                        }
                        enqueueCompletedLoadResult(
                            pendingLoads,
                            domain,
                            key,
                            cacheKey,
                            group,
                            priority,
                            std::move(loadResult));
                    }
                    requestState.completeContentRequest(cacheKey);
                }
                condition.notify_all();
            },
            toHttpPriority(group));
        return TileLoadDispatchResult::Issued;
    }

private:
    static void enqueueCompletedLoadResult(
        TilePendingLoadQueue& pendingLoads,
        TileLoadDomain domain,
        const TileKey& key,
        const std::string& cacheKey,
        TileLoadPriorityGroup group,
        double priority,
        TileLoadResult loadResult) {
        PendingTileLoad pending{
            domain,
            key,
            cacheKey,
            group,
            priority,
            std::move(loadResult)};
        if (pending.result.shouldUpload()) {
            pendingLoads.addUpload(std::move(pending));
        } else {
            pendingLoads.addTerminalResult(std::move(pending));
        }
    }

    static HttpRequestPriority toHttpPriority(TileLoadPriorityGroup group) {
        switch (group) {
            case TileLoadPriorityGroup::Preload:
                return HttpRequestPriority::Low;
            case TileLoadPriorityGroup::Normal:
                return HttpRequestPriority::Normal;
            case TileLoadPriorityGroup::Urgent:
                return HttpRequestPriority::High;
        }
        return HttpRequestPriority::Normal;
    }
};

} // namespace earth_engine
