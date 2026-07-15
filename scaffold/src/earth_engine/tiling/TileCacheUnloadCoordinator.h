#pragma once

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
#include <vector>

namespace earth_engine {

enum class TileCacheUnloadContentResult {
    Keep,
    Remove,
    RemoveAndClearChildren
};

struct TileCacheUnloadResult {
    int64_t totalBytesUsed = 0;
    size_t unloadedTiles = 0;
};

class TileCacheUnloadCoordinator {
public:
    template <typename CurrentTotalBytesFn,
              typename ShouldContinueFn,
              typename SubtreeHasActiveContentWorkFn,
              typename UnloadTileContentFn,
              typename MarkIneligibleFn,
              typename ClearChildrenFn,
              typename TrimExternalCachesFn>
    static TileCacheUnloadResult run(
        TileUnloadQueue& unloadQueue,
        const std::unordered_map<
            std::string,
            std::unique_ptr<TilesetTile>>& tiles,
        int64_t maximumCachedBytes,
        double timeBudgetMs,
        CurrentTotalBytesFn&& currentTotalBytes,
        ShouldContinueFn&& shouldContinue,
        SubtreeHasActiveContentWorkFn&& subtreeHasActiveContentWork,
        UnloadTileContentFn&& unloadTileContent,
        MarkIneligibleFn&& markIneligible,
        ClearChildrenFn&& clearChildren,
        TrimExternalCachesFn&& trimExternalCaches) {
        const double unloadStartMs = perf::nowMs();
        const auto timeBudgetExpired = [&]() {
            return timeBudgetMs > 0.0 &&
                   perf::nowMs() - unloadStartMs >= timeBudgetMs;
        };

        std::vector<TilesetTile*> tilesNeedingChildrenCleared;
        size_t unloadedTiles = 0;
        trimExternalCaches();
        size_t remainingCandidates = unloadQueue.size();
        while (shouldContinue(maximumCachedBytes) &&
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

            if (tile.content.loadState != TileLoadState::Unloading &&
                !TileUnloadPolicy::isEligibleForContentUnloadQueue(tile)) {
                markIneligible(key);
                if (timeBudgetExpired()) break;
                continue;
            }

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
            trimExternalCaches();
            if (removed == TileCacheUnloadContentResult::Keep) {
                unloadQueue.moveFrontToBack();
                if (timeBudgetExpired()) break;
                continue;
            }

            markIneligible(key);
            if (removed ==
                TileCacheUnloadContentResult::RemoveAndClearChildren) {
                tilesNeedingChildrenCleared.emplace_back(&tile);
            }
            ++unloadedTiles;

            if (timeBudgetExpired()) break;
        }

        for (TilesetTile* tile : tilesNeedingChildrenCleared) {
            if (tile) {
                clearChildren(*tile);
            }
        }

        return TileCacheUnloadResult{
            currentTotalBytes(),
            unloadedTiles};
    }
};

} // namespace earth_engine
