#pragma once

#include "TileRenderPlanFrameRefresher.h"
#include "TileSelectionFrameFinalizer.h"

#include <string>
#include <unordered_set>
#include <vector>

namespace earth_engine {

class ActivatedRasterOverlay;
class TileContentAccess;
class TilesetTileRegistry;
struct TilePlan;
struct TilesetTile;
struct TileSelectionCounters;

struct TileSelectionFrameFinalizationInput {
    TilePlan& tilePlan;
    TilesetTileRegistry& tileRegistry;
    const std::vector<TilesetTile*>& activeTiles;
    const std::vector<TilesetTile*>& previousActiveTiles;
    TileSelectionCounters& selectionCounters;
    TileContentAccess& contentAccess;
    const std::vector<ActivatedRasterOverlay*>& rasterOverlays;
    TileRenderPlanFrameRefreshOptions renderPlanOptions;
};

class TileSelectionFrameFinalizationRunner {
public:
    static TileSelectionFrameFinalizeTimings finalize(
        TileSelectionFrameFinalizationInput input);
};

} // namespace earth_engine
