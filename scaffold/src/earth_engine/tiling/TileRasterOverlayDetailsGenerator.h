#pragma once

#include "SurfaceTile.h"

namespace earth_engine {

class TileRenderContentState;
struct TileBoundingVolume;

class TileRasterOverlayDetailsGenerator {
public:
    static bool ensureProjectionDetailsFromRegion(
        TileRenderContentState& renderContent,
        const TileBoundingVolume& boundingVolume,
        RasterOverlayProjection projection);
};

} // namespace earth_engine
