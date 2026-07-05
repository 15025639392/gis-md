#pragma once

#include "TileSelectionTraversalContext.h"

namespace earth_engine {

class TileContentAccess;
class Tileset;
enum class TileOcclusionState;

struct TileSelectionTraversalContextBuildInput {
    TilePlan& tilePlan;
    TileLoadQueue& loadQueue;
    TileSelectionCounters& counters;
    const TilesetOptions& options;
    const std::vector<ActivatedRasterOverlay*>& rasterOverlays;
    RenderDevice* device = nullptr;
    FrameResourceBudget& frameResourceBudget;
    Vec3 lastCameraPosition = Vec3::zero();
    TileContentAccess& contentAccess;
};

struct TileSelectionTraversalContextBinding {
    using CheckOcclusionFn =
        TileOcclusionState (*)(void*, const TilesetTile&);

    TilePlan& tilePlan;
    TileLoadQueue& loadQueue;
    const TilesetOptions& options;
    const std::vector<ActivatedRasterOverlay*>& rasterOverlays;
    TileContentAccess& contentAccess;
    void* occlusionUserData = nullptr;
    CheckOcclusionFn checkOcclusion = nullptr;
    Tileset* owner = nullptr;
};

class TileSelectionTraversalContextBuilder {
public:
    static TileSelectionTraversalContext build(
        TileSelectionTraversalContextBuildInput input,
        TileSelectionTraversalContextBinding& binding);
};

} // namespace earth_engine
