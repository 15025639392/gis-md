#pragma once

#include "TileCacheMetrics.h"
#include "TileLoadState.h"
#include "TileUnloadPolicy.h"
#include "TileUnloadQueue.h"
#include "TilesetTile.h"

#include "../debug/PerfTimer.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

namespace earth_engine {

enum class TileCacheUnloadContentResult {
    Keep,
    Remove,
    RemoveAndClearChildren
};

struct TileCacheUnloadResult {
    int64_t totalBytesUsed = 0;
    bool cacheBytesDirty = false;
    bool shouldRefreshTotalBytes = false;
};

class TileCacheUnloadCoordinator {
public:
    template <typename SubtreeHasActiveContentWorkFn,
              typename UnloadTileContentFn,
              typename MarkIneligibleFn,
              typename ClearChildrenFn>
    static TileCacheUnloadResult run(
        TileUnloadQueue& unloadQueue,
        const std::unordered_map<
            std::string,
            std::unique_ptr<TilesetTile>>& tiles,
        int64_t totalBytesUsed,
        int64_t maximumCachedBytes,
        double timeBudgetMs,
        bool resourceSmoothingActive,
        bool cacheBytesDirty,
        SubtreeHasActiveContentWorkFn&& subtreeHasActiveContentWork,
        UnloadTileContentFn&& unloadTileContent,
        MarkIneligibleFn&& markIneligible,
        ClearChildrenFn&& clearChildren) {
        const double unloadStartMs = perf::nowMs();
        const auto timeBudgetExpired = [&]() {
            return timeBudgetMs > 0.0 &&
                   perf::nowMs() - unloadStartMs >= timeBudgetMs;
        };
        const auto hasQueuedUnloadingTile = [&]() {
            return TileUnloadPolicy::hasQueuedTileInState(
                unloadQueue,
                tiles,
                TileLoadState::Unloading);
        };

        size_t remainingCandidates = unloadQueue.size();
        while ((totalBytesUsed > maximumCachedBytes ||
                hasQueuedUnloadingTile()) &&
               !unloadQueue.empty() &&
               remainingCandidates > 0) {
            --remainingCandidates;
            const std::string key = unloadQueue.front();
            auto tileIt = tiles.find(key);
            if (tileIt == tiles.end()) {
                unloadQueue.popFront();
                if (timeBudgetExpired()) break;
                continue;
            }

            TilesetTile& tile = *tileIt->second;
            const int64_t estimatedBytesBeforeUnload =
                TileCacheMetrics::estimateTileBytes(tile);

            const bool externalSubtreeHasActiveWork =
                tile.content.contentKind == TileContentKind::External &&
                subtreeHasActiveContentWork(tile);
            if (TileUnloadPolicy::shouldDeferForReferences(
                    tile,
                    externalSubtreeHasActiveWork)) {
                unloadQueue.moveFrontToBack();
                if (timeBudgetExpired()) break;
                continue;
            }

            const TileCacheUnloadContentResult removed =
                unloadTileContent(tile);
            if (removed == TileCacheUnloadContentResult::Keep) {
                unloadQueue.moveFrontToBack();
                if (timeBudgetExpired()) break;
                continue;
            }

            markIneligible(key);
            if (removed ==
                TileCacheUnloadContentResult::RemoveAndClearChildren) {
                clearChildren(tile);
            }

            totalBytesUsed = std::max<int64_t>(
                0,
                totalBytesUsed -
                    std::max<int64_t>(0, estimatedBytesBeforeUnload));
            cacheBytesDirty = true;

            if (timeBudgetExpired()) break;
        }

        return TileCacheUnloadResult{
            totalBytesUsed,
            cacheBytesDirty,
            cacheBytesDirty && !resourceSmoothingActive};
    }
};

} // namespace earth_engine
