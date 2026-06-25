#pragma once

#include "TileContentUpsampleKind.h"
#include "TileRenderContentState.h"
#include "TilesetTile.h"

namespace earth_engine {

struct TileSurfaceMeshResolution {
    SurfaceDrawableSource source = SurfaceDrawableSource::None;
    bool markDone = false;

    static TileSurfaceMeshResolution forContext(
        bool hasOwnTerrain,
        TileContentUpsampleKind upsampleKind,
        bool hasTerrainQuadtree) {
        TileSurfaceMeshResolution resolution;
        resolution.markDone =
            hasOwnTerrain ||
            upsampleKind != TileContentUpsampleKind::None ||
            !hasTerrainQuadtree;
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
        return tile.content.renderContent.needsHeightmapSurfaceReplacement(
            hasOwnTerrain);
    }
};

} // namespace earth_engine
