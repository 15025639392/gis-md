#pragma once

#include "TileLodTransitionFrameUpdater.h"
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
    TileSelectionCounters& selectionCounters;
    TileContentAccess& contentAccess;
    std::unordered_set<std::string>& fadingKeys;
    const std::vector<ActivatedRasterOverlay*>& rasterOverlays;
    double deltaSeconds = 0.0;
    TileLodTransitionFrameOptions lodTransitionOptions;
    TileRenderPlanFrameRefreshOptions renderPlanOptions;
};

class TileSelectionFrameFinalizationRunner {
public:
    static TileSelectionFrameFinalizeTimings finalize(
        TileSelectionFrameFinalizationInput input);
};

} // namespace earth_engine
