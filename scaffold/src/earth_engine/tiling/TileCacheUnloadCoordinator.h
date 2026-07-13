#pragma once

#include "TileCacheMetrics.h"
#include "TileLoadState.h"
#include "RasterMappedToTilesetTile.h"
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
    bool cacheBytesDirty = false;
    bool shouldRefreshTotalBytes = false;
};

class TileCacheUnloadCoordinator {
public:
    static bool requiresImmediateByteRefreshAfterUnload(
        const TilesetTile& tile) {
        bool usesAncestorRasterFallback = false;
        tile.rasterOverlayState.forEachMapping([&](const auto* mapping) {
            if (usesAncestorRasterFallback || !mapping) {
                return;
            }
            usesAncestorRasterFallback =
                mapping->getReadyTileSource() ==
                RasterMappedToTilesetTile::ReadyTileSource::Ancestor;
        });
        return usesAncestorRasterFallback;
    }

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
        const auto hasQueuedTileNeedingUnload = [&]() {
            for (const std::string& k : unloadQueue.keys()) {
                auto it = tiles.find(k);
                if (it != tiles.end() && it->second) {
                    const TileLoadState s =
                        it->second->content.loadState;
                    if (s == TileLoadState::Done ||
                        s == TileLoadState::ContentLoaded ||
                        s == TileLoadState::Failed ||
                        s == TileLoadState::Unloading) {
                        return true;
                    }
                }
            }
            return false;
        };

        std::vector<TilesetTile*> tilesNeedingChildrenCleared;
        bool immediateByteRefreshRequired = false;
        size_t remainingCandidates = unloadQueue.size();
        while ((totalBytesUsed > maximumCachedBytes ||
                hasQueuedUnloadingTile() ||
                hasQueuedTileNeedingUnload()) &&
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
            const bool needsImmediateRefresh =
                requiresImmediateByteRefreshAfterUnload(tile);

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
                tilesNeedingChildrenCleared.emplace_back(&tile);
            }

            immediateByteRefreshRequired =
                immediateByteRefreshRequired || needsImmediateRefresh;
            totalBytesUsed = std::max<int64_t>(
                0,
                totalBytesUsed -
                    std::max<int64_t>(0, estimatedBytesBeforeUnload));
            cacheBytesDirty = true;

            if (timeBudgetExpired()) break;
        }

        for (TilesetTile* tile : tilesNeedingChildrenCleared) {
            if (tile) {
                clearChildren(*tile);
            }
        }

        return TileCacheUnloadResult{
            totalBytesUsed,
            cacheBytesDirty,
            cacheBytesDirty &&
                (!resourceSmoothingActive || immediateByteRefreshRequired)};
    }
};

} // namespace earth_engine
