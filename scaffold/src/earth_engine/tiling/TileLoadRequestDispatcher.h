#pragma once

#include "TileLoadTypes.h"
#include "TilePendingLoadQueue.h"
#include "TilePendingRequestState.h"
#include "TileLoadPriorityPolicy.h"
#include "../core/resources/FrameResourceBudget.h"
#include "../content/GltfContentProvider.h"
#include "../providers/TerrainProvider.h"
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
    static TileLoadDispatchResult queueUpsampledTerrain(
        std::mutex& mutex,
        TilePendingRequestState& requestState,
        TilePendingLoadQueue& pendingLoads,
        const TileKey& key,
        const std::string& cacheKey,
        TileLoadPriorityGroup group,
        double priority,
        TileLoadDomain domain = TileLoadDomain::Terrain,
        TileLoadResult result = TileLoadResult::createRenderable());

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
                    FrameResourceLane::ContentRequest,
                    TileLoadPriorityPolicy::toFramePriority(group),
                    estimatedFanout)) {
                return TileLoadDispatchResult::Blocked;
            }
            if (!requestState.beginContentRequest(cacheKey, token)) {
                return TileLoadDispatchResult::Skipped;
            }
        }

        onIssued();
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
             priority](const TileKey&, TileContentLoadResult result) mutable {
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (!requestState.destroying() && !token.isCancelled()) {
                        enqueueCompletedLoadResult(
                            pendingLoads,
                            TileLoadDomain::Content,
                            key,
                            cacheKey,
                            group,
                            priority,
                            TileLoadResult::fromContentResult(
                                std::move(result)));
                    }
                    requestState.completeContentRequest(cacheKey);
                }
                condition.notify_all();
            },
            toHttpPriority(group));
        return TileLoadDispatchResult::Issued;
    }

    template <typename OnIssuedFn>
    static TileLoadDispatchResult requestTerrain(
        std::mutex& mutex,
        std::condition_variable& condition,
        TilePendingRequestState& requestState,
        TilePendingLoadQueue& pendingLoads,
        FrameResourceBudget& budget,
        TerrainProvider& provider,
        const TileKey& key,
        const std::string& cacheKey,
        TileLoadPriorityGroup group,
        double priority,
        OnIssuedFn&& onIssued) {
        CancellationToken token;
        const int estimatedFanout = provider.estimatedRequestFanout(key);
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
                    FrameResourceLane::TerrainRequest,
                    TileLoadPriorityPolicy::toFramePriority(group),
                    estimatedFanout)) {
                return TileLoadDispatchResult::Blocked;
            }
            if (!requestState.beginTerrainRequest(cacheKey, token)) {
                return TileLoadDispatchResult::Skipped;
            }
        }

        onIssued();
        provider.requestTile(
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
             priority](const TileKey&, TerrainTileLoadResult result) mutable {
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (!requestState.destroying() && !token.isCancelled()) {
                        TileLoadResult loadResult =
                            TileLoadResult::fromTerrainResult(
                                std::move(result));
                        enqueueCompletedLoadResult(
                            pendingLoads,
                            TileLoadDomain::Terrain,
                            key,
                            cacheKey,
                            group,
                            priority,
                            std::move(loadResult));
                    }
                    requestState.completeTerrainRequest(cacheKey);
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
