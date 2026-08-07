#pragma once

#include "TileKey.h"
#include "TileLoadTypes.h"

namespace earth_engine {

class TileLoadQueue;
struct TilePlan;
struct TilesetTile;

class TileSelectionPlanAppender {
public:
    static void queueTileLoad(TileLoadQueue& loadQueue,
                              const TileKey& key,
                              TileLoadPriorityGroup group,
                              double priority);

    static void addTileToCurrentPlan(TilePlan& tilePlan,
                                     TileLoadQueue& loadQueue,
                                     TilesetTile& tile,
                                     double tileSse,
                                     bool queueForLoad,
                                     double tilePriority);
};

} // namespace earth_engine
