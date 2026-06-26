#pragma once

#include "SurfaceMeshResourcePreparer.h"
#include "TileRenderContentState.h"
#include "TileTerrainHeightRangePolicy.h"
#include "TilesetTile.h"

namespace earth_engine {

struct DecodedHeightmap;
class RenderDevice;

struct TileSurfaceRenderContentCommit {
    SurfaceDrawableSource source = SurfaceDrawableSource::None;
    bool markDone = false;
    const DecodedHeightmap* ownHeightmap = nullptr;
    RenderDevice* device = nullptr;
};

class TileSurfaceRenderContentCoordinator {
public:
    template <typename IsCompleteRenderableFn>
    static void commitSurface(TilesetTile& tile,
                              const TileSurfaceRenderContentCommit& commit,
                              IsCompleteRenderableFn&&
                                  isCompleteRenderable) {
        SurfaceMeshResourcePreparer::prepare(tile, commit.device);
        TileTerrainHeightRangePolicy::applyMeshOrHeightmapRange(
            tile,
            tile.content.renderContent.surfaceMesh(),
            commit.ownHeightmap);
        TileTerrainHeightRangePolicy::inheritHeightRangeForUnreadyChildren(
            tile);
        tile.commitSurfaceRenderContent(
            commit.source,
            commit.markDone,
            isCompleteRenderable);
    }
};

} // namespace earth_engine
