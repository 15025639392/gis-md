#pragma once

#include "TileOcclusionState.h"

#include <functional>

namespace earth_engine {

struct TilesetTile;

using TileOcclusionCallback =
    std::function<TileOcclusionState(const TilesetTile&)>;

} // namespace earth_engine
