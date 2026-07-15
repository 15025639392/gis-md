#pragma once

#include "TileCacheKey.h"
#include "TileRenderFrameBuilder.h"
#include "TileRenderFrameInputBuilder.h"
#include <utility>
#include <vector>

namespace earth_engine {

struct TileRenderFrameCoordinatorInput {
    TilePlan& tilePlan;
    std::vector<ActivatedRasterOverlay*>& rasterOverlays;
    uint64_t frameNumber = 0;
    Vec3 lastCameraPosition;
    const std::vector<FogDensityAtHeight>& fogDensityTable;
    int fogCulled = 0;
    bool resourceSmoothingActive = false;
    bool interactionActive = false;
};

class TileRenderFrameCoordinator {
public:
    template <typename MarkIneligibleFn,
              typename TrackReferenceFn,
              typename BuildTileDrawCommandFn,
              typename TrimRasterCachesFn,
              typename ShouldUnloadCachedBytesFn,
              typename UnloadCachedBytesFn>
    static void run(
        TileRenderFrameCoordinatorInput input,
        Renderer& renderer,
        RenderCommandList& commands,
        MarkIneligibleFn&& markIneligible,
        TrackReferenceFn&& trackReference,
        BuildTileDrawCommandFn&& buildTileDrawCommand,
        TrimRasterCachesFn&& trimRasterCaches,
        ShouldUnloadCachedBytesFn&& shouldUnloadCachedBytes,
        UnloadCachedBytesFn&& unloadCachedBytes) {
        TileRenderFrameBuilder::build(
            TileRenderFrameInputBuilder::build(
                input.tilePlan,
                input.rasterOverlays,
                input.frameNumber,
                input.lastCameraPosition,
                input.fogDensityTable,
                input.fogCulled,
                input.resourceSmoothingActive,
                input.interactionActive),
            renderer,
            commands,
            [](const TileKey& key) {
                return TileCacheKey::forTile(key);
            },
            std::forward<MarkIneligibleFn>(markIneligible),
            std::forward<TrackReferenceFn>(trackReference),
            std::forward<BuildTileDrawCommandFn>(buildTileDrawCommand),
            std::forward<TrimRasterCachesFn>(trimRasterCaches),
            std::forward<ShouldUnloadCachedBytesFn>(
                shouldUnloadCachedBytes),
            std::forward<UnloadCachedBytesFn>(unloadCachedBytes));
    }
};

} // namespace earth_engine
