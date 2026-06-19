#pragma once

#include "TileKey.h"
#include "TileSelectionFrameFinalizer.h"

namespace earth_engine {

struct FrameState;
struct TilesetTile;
struct TilesetTestAccess;
class Tileset;
enum class TileLoadPriorityGroup;

class TilesetSelectionFrameFacade {
public:
    static void selectTiles(Tileset& tileset, const FrameState& frameState);
    static void refreshTilePlanRenderEntries(Tileset& tileset);

private:
    friend struct TilesetTestAccess;

    static void resetTileSelectionState(Tileset& tileset);
    static void queueTileLoad(Tileset& tileset,
                              const TileKey& key,
                              TileLoadPriorityGroup group,
                              double priority);
    static void addTileToCurrentPlan(Tileset& tileset,
                                     TilesetTile& tile,
                                     double tileSse,
                                     bool queueForLoad,
                                     double tilePriority);
    static bool hasLodTransitionRenderContent(const Tileset& tileset,
                                              const TilesetTile& tile);
    static void updateLodTransitions(Tileset& tileset,
                                     double deltaSeconds);
    static TileSelectionFrameFinalizeTimings finalizeSelectedTilePlan(
        Tileset& tileset,
        const FrameState& frameState);
};

} // namespace earth_engine
