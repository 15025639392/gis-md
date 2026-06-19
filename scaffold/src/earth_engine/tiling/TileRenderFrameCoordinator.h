#pragma once

#include "TileCacheKey.h"
#include "TileRenderFrameBuilder.h"
#include "TileRenderFrameInputBuilder.h"
#include "TileSubtreeRemovalCoordinator.h"

#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

namespace earth_engine {

struct TileRenderFrameCoordinatorInput {
    TilePlan& tilePlan;
    const std::unordered_map<std::string, std::unique_ptr<TilesetTile>>& tiles;
    TileUnloadQueue& unloadQueue;
    std::vector<ActivatedRasterOverlay*>& rasterOverlays;
    bool& cacheBytesDirty;
    uint64_t frameNumber = 0;
    Vec3 lastCameraPosition;
    const std::vector<FogDensityAtHeight>& fogDensityTable;
    int fogCulled = 0;
    bool resourceSmoothingActive = false;
    bool interactionActive = false;
    int64_t totalBytesUsed = 0;
    int64_t maximumCachedBytes = 0;
};

class TileRenderFrameCoordinator {
public:
    template <typename EnsureTileFn,
              typename MarkIneligibleFn,
              typename BuildTileDrawCommandFn,
              typename MarkEligibleFn,
              typename UpdateTotalBytesFn,
              typename UnloadCachedBytesFn>
    static void run(
        TileRenderFrameCoordinatorInput input,
        Renderer& renderer,
        RenderCommandList& commands,
        EnsureTileFn&& ensureTile,
        MarkIneligibleFn&& markIneligible,
        BuildTileDrawCommandFn&& buildTileDrawCommand,
        MarkEligibleFn&& markEligible,
        UpdateTotalBytesFn&& updateTotalBytes,
        UnloadCachedBytesFn&& unloadCachedBytes) {
        TileRenderFrameBuilder::build(
            TileRenderFrameInputBuilder::build(
                input.tilePlan,
                input.tiles,
                input.unloadQueue,
                input.rasterOverlays,
                input.cacheBytesDirty,
                input.frameNumber,
                input.lastCameraPosition,
                input.fogDensityTable,
                input.fogCulled,
                input.resourceSmoothingActive,
                input.interactionActive,
                input.totalBytesUsed,
                input.maximumCachedBytes),
            renderer,
            commands,
            std::forward<EnsureTileFn>(ensureTile),
            [](const TileKey& key) {
                return TileCacheKey::forTile(key);
            },
            std::forward<MarkIneligibleFn>(markIneligible),
            std::forward<BuildTileDrawCommandFn>(buildTileDrawCommand),
            [&renderer](
                const std::vector<TileFrameInactiveEntry>& inactiveTiles) {
                TileSubtreeRemovalCoordinator::detachInactiveRasterOverlays(
                    inactiveTiles,
                    &renderer);
            },
            std::forward<MarkEligibleFn>(markEligible),
            std::forward<UpdateTotalBytesFn>(updateTotalBytes),
            std::forward<UnloadCachedBytesFn>(unloadCachedBytes));
    }
};

} // namespace earth_engine
