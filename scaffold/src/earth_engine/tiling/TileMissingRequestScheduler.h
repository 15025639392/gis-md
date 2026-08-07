#pragma once

#include "TileEmptyContentRegistry.h"
#include "TileLoadLifecycle.h"
#include "TileLoadScheduler.h"
#include "TilesetTile.h"
#include "../core/resources/FrameResourceBudget.h"
#include "../content/GltfContentProvider.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace earth_engine {

struct TileMissingRequestSchedulerInput {
    TileLoadLifecycle& loadLifecycle;
    FrameResourceBudget& budget;
    TilesetContentProvider* contentProvider = nullptr;
    const std::unordered_map<std::string, std::unique_ptr<TilesetTile>>& tiles;
    const TileEmptyContentRegistry& emptyContentRegistry;
    const std::vector<RasterOverlayProjection>*
        requiredRasterOverlayProjections = nullptr;
};

class TileMissingRequestScheduler {
public:
    template <typename TerrainCacheKeyFn,
              typename PrepareUpsampleSourceTileFn,
              typename EnsureTileFn>
    static TileLoadRequestOutcome request(
        const std::vector<TileLoadRequest>& loadRequests,
        TileMissingRequestSchedulerInput input,
        TerrainCacheKeyFn&& terrainCacheKey,
        PrepareUpsampleSourceTileFn&& prepareUpsampleSourceTile,
        EnsureTileFn&& ensureTile) {
        return requestImpl(
            loadRequests,
            input,
            std::forward<TerrainCacheKeyFn>(terrainCacheKey),
            std::forward<PrepareUpsampleSourceTileFn>(
                prepareUpsampleSourceTile),
            std::forward<EnsureTileFn>(ensureTile));
    }

    template <typename TerrainCacheKeyFn,
              typename PrepareUpsampleSourceTileFn,
              typename EnsureTileFn>
    static TileLoadRequestOutcome request(
        TileLoadQueue& loadQueue,
        TileMissingRequestSchedulerInput input,
        TerrainCacheKeyFn&& terrainCacheKey,
        PrepareUpsampleSourceTileFn&& prepareUpsampleSourceTile,
        EnsureTileFn&& ensureTile) {
        return requestImpl(
            loadQueue,
            input,
            std::forward<TerrainCacheKeyFn>(terrainCacheKey),
            std::forward<PrepareUpsampleSourceTileFn>(
                prepareUpsampleSourceTile),
            std::forward<EnsureTileFn>(ensureTile));
    }

private:
    template <typename LoadRequests,
              typename TerrainCacheKeyFn,
              typename PrepareUpsampleSourceTileFn,
              typename EnsureTileFn>
    static TileLoadRequestOutcome requestImpl(
        LoadRequests& loadRequests,
        TileMissingRequestSchedulerInput input,
        TerrainCacheKeyFn&& terrainCacheKey,
        PrepareUpsampleSourceTileFn&& prepareUpsampleSourceTile,
        EnsureTileFn&& ensureTile) {
        return TileLoadScheduler::requestMissingTiles(
            loadRequests,
            TileLoadSchedulerInput{
                input.loadLifecycle,
                input.budget,
                input.contentProvider,
                input.requiredRasterOverlayProjections},
            terrainCacheKey,
            [&](const TileKey& key,
                const std::string& cacheKey,
                TilesetTile*& tileState) {
                return makeSnapshot(
                    input,
                    key,
                    cacheKey,
                    tileState);
            },
            [&](const std::string& cacheKey) {
                return input.emptyContentRegistry.contains(cacheKey);
            },
            prepareUpsampleSourceTile,
            [&](const TileKey& key) {
                if (TilesetTile* tile = ensureTile(key)) {
                    tile->markContentLoading();
                }
            });
    }

    static TileLoadRequestSnapshot makeSnapshot(
        const TileMissingRequestSchedulerInput& input,
        const TileKey& key,
        const std::string& cacheKey,
        TilesetTile*& outTileState) {
        auto tileStateIt = input.tiles.find(cacheKey);
        outTileState = tileStateIt != input.tiles.end()
            ? tileStateIt->second.get()
            : nullptr;

        TileLoadRequestSnapshot snapshot;
        snapshot.hasTile = outTileState != nullptr;
        snapshot.upsampleKind =
            outTileState != nullptr
                ? outTileState->content.upsampleKind()
                : TileContentUpsampleKind::None;
        snapshot.contentProviderSupportsTile =
            snapshot.upsampleKind == TileContentUpsampleKind::None &&
            input.contentProvider &&
            input.contentProvider->supportsTile(key);
        snapshot.contentProviderOwnsTerrainQuadtree =
            input.contentProvider &&
            input.contentProvider->providesTerrainQuadtree();
        snapshot.hasRenderContent =
            outTileState &&
            outTileState->hasCommittedRenderContent();
        if (outTileState) {
            snapshot.loadState = outTileState->content.loadState;
        }
        return snapshot;
    }
};

} // namespace earth_engine
