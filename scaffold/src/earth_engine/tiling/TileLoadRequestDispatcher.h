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
        FrameResourceBudget& budget,
        const TileKey& key,
        const std::string& cacheKey,
        TileLoadPriorityGroup group,
        double priority);

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
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (requestState.destroying()) {
                return TileLoadDispatchResult::Destroying;
            }
            if (requestState.contains(cacheKey) ||
                pendingLoads.hasContentUpload(cacheKey)) {
                return TileLoadDispatchResult::Skipped;
            }
            if (!budget.tryIssue(
                    FrameResourceLane::ContentRequest,
                    TileLoadPriorityPolicy::toFramePriority(group))) {
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
                        if (result.status == TileContentLoadStatus::Render &&
                            result.gltfModel) {
                            pendingLoads.addContentUpload(
                                PendingContentUpload{
                                    key,
                                    cacheKey,
                                    group,
                                    priority,
                                    std::move(result)});
                        } else {
                            pendingLoads.addContentTerminalResult(
                                PendingContentTerminalResult{
                                    key,
                                    cacheKey,
                                    group,
                                    priority,
                                    result.status});
                        }
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
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (requestState.destroying()) {
                return TileLoadDispatchResult::Destroying;
            }
            if (requestState.contains(cacheKey) ||
                pendingLoads.hasTerrainUpload(cacheKey) ||
                pendingLoads.hasContentUpload(cacheKey)) {
                return TileLoadDispatchResult::Skipped;
            }
            if (!budget.tryIssue(
                    FrameResourceLane::TerrainRequest,
                    TileLoadPriorityPolicy::toFramePriority(group))) {
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
                        if (result.status == TerrainTileLoadStatus::Success &&
                            result.heightmap) {
                            pendingLoads.addTerrainUpload(
                                PendingTerrainUpload{
                                    key,
                                    cacheKey,
                                    group,
                                    priority,
                                    std::move(result.heightmap)});
                        } else {
                            pendingLoads.addTerrainTerminalResult(
                                PendingTerrainTerminalResult{
                                    key,
                                    cacheKey,
                                    group,
                                    priority,
                                    result.status});
                        }
                    }
                    requestState.completeTerrainRequest(cacheKey);
                }
                condition.notify_all();
            },
            toHttpPriority(group));
        return TileLoadDispatchResult::Issued;
    }

private:
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
