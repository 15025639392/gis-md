#pragma once

namespace earth_engine {

struct TilePlan;

class TileVisibleRangeFinalizer {
public:
    static void dedupeVisibleTiles(TilePlan& plan);
    static void updateVisibleZoomRange(TilePlan& plan);
};

} // namespace earth_engine
