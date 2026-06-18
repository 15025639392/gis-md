#pragma once

#include "TileOcclusionState.h"

#include "../core/math/Vec3.h"

namespace earth_engine {

struct TilesetTile;

class TileSoftwareOcclusionPolicy {
public:
    static TileOcclusionState check(const TilesetTile& tile,
                                    const Vec3& cameraPosition);
};

} // namespace earth_engine
