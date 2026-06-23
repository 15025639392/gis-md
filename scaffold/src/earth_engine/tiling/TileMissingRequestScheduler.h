#pragma once

#include "LegacyHeightmapTerrainCacheMode.h"
#include "TileEmptyContentRegistry.h"
#include "TileLoadLifecycle.h"
#include "TileLoadScheduler.h"
#include "TilesetTile.h"
#include "../core/resources/FrameResourceBudget.h"
#include "../content/GltfContentProvider.h"
#include "../terrain/TerrainTile.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace earth_engine {

struct TileMissingRequestSchedulerInput {
    TileLoadLifecycle& loadLifecycle;
    FrameResourceBudget& budget;
    TilesetContentProvider* contentProvider = nullptr;
    const std::unordered_map<std::string, std::unique_ptr<TilesetTile>>& tiles;
    const std::unordered_map<std::string, std::unique_ptr<DecodedHeightmap>>&
        terrainCache;
    LegacyHeightmapTerrainCacheMode legacyHeightmapCacheMode =
        LegacyHeightmapTerrainCacheMode::Include;
    const TileEmptyContentRegistry& emptyContentRegistry;
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
        return TileLoadScheduler::requestMissingTiles(
            loadRequests,
            TileLoadSchedulerInput{
                input.loadLifecycle,
                input.budget,
                input.contentProvider},
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

private:
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
        snapshot.upsampledFromParent =
            outTileState != nullptr &&
            outTileState->content.derivesTerrainFromParent();
        snapshot.contentProviderSupportsTile =
            !snapshot.upsampledFromParent &&
            input.contentProvider &&
            input.contentProvider->supportsTile(key);
        snapshot.contentProviderOwnsTerrainQuadtree =
            input.contentProvider &&
            input.contentProvider->providesTerrainQuadtree();
        const bool legacyHeightmapCacheCanSatisfyRequest =
            input.legacyHeightmapCacheMode ==
            LegacyHeightmapTerrainCacheMode::Include;
        snapshot.terrainAlreadyCached =
            legacyHeightmapCacheCanSatisfyRequest &&
            !snapshot.contentProviderOwnsTerrainQuadtree &&
            input.terrainCache.count(cacheKey) > 0;
        snapshot.hasRenderContent =
            outTileState &&
            outTileState->content.contentKind == TileContentKind::Render &&
            outTileState->content.renderContent.hasGltfContent();
        if (outTileState) {
            snapshot.loadState = outTileState->content.loadState;
        }
        return snapshot;
    }
};

} // namespace earth_engine
