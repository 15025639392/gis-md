#pragma once

#include "TileRenderContentState.h"
#include "TilesetTile.h"

namespace earth_engine {

struct TileSurfaceMeshResolution {
    SurfaceDrawableSource source = SurfaceDrawableSource::None;
    bool markDone = false;

    static TileSurfaceMeshResolution forContext(
        bool hasOwnTerrain,
        bool upsampledFromParent,
        bool hasTerrainProvider) {
        TileSurfaceMeshResolution resolution;
        resolution.markDone =
            hasOwnTerrain || upsampledFromParent || !hasTerrainProvider;
        return resolution;
    }

    SurfaceDrawableSource resolvedSource() const {
        return source == SurfaceDrawableSource::None
            ? SurfaceDrawableSource::EllipsoidFallback
            : source;
    }
};

class TileSurfaceMeshResolutionPolicy {
public:
    static bool shouldReplaceReadySurface(const TilesetTile& tile,
                                          bool hasOwnTerrain) {
        return tile.content.renderContent.needsOwnTerrainSurfaceReplacement(
            hasOwnTerrain);
    }
};

} // namespace earth_engine
