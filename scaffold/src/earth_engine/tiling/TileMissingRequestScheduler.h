#pragma once

#include "TileEmptyContentRegistry.h"
#include "TileLoadLifecycle.h"
#include "TileLoadScheduler.h"
#include "TileSelectionRootPolicy.h"
#include "TilesetTile.h"
#include "../core/resources/FrameResourceBudget.h"
#include "../content/GltfContentProvider.h"
#include "../providers/TerrainProvider.h"
#include "../terrain/TerrainTile.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace earth_engine {

struct TileMissingRequestSchedulerInput {
    TileLoadLifecycle& loadLifecycle;
    FrameResourceBudget& budget;
    TerrainProvider* legacyTerrainProvider = nullptr;
    TilesetContentProvider* contentProvider = nullptr;
    const std::unordered_map<std::string, std::unique_ptr<TilesetTile>>& tiles;
    const std::unordered_map<std::string, std::unique_ptr<DecodedHeightmap>>&
        terrainCache;
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
        TerrainProvider* legacyTerrainProvider =
            effectiveLegacyTerrainProvider(input);
        return TileLoadScheduler::requestMissingTiles(
            loadRequests,
            TileLoadSchedulerInput{
                input.loadLifecycle,
                input.budget,
                legacyTerrainProvider,
                input.contentProvider},
            terrainCacheKey,
            [&](const TileKey& key,
                const std::string& cacheKey,
                TilesetTile*& tileState) {
                return makeSnapshot(
                    input,
                    legacyTerrainProvider,
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
    static TerrainProvider* effectiveLegacyTerrainProvider(
        const TileMissingRequestSchedulerInput& input) {
        if (input.contentProvider &&
            input.contentProvider->providesTerrainQuadtree()) {
            return nullptr;
        }
        return input.legacyTerrainProvider;
    }

    static TileLoadRequestSnapshot makeSnapshot(
        const TileMissingRequestSchedulerInput& input,
        const TerrainProvider* terrainProvider,
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
        snapshot.legacyTerrainProviderSupportsTile =
            !snapshot.contentProviderOwnsTerrainQuadtree &&
            !TileSelectionRootPolicy::isVirtualTerrainRoot(key) &&
            terrainProvider &&
            terrainProvider->supportsTile(key);
        snapshot.terrainAlreadyCached =
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
