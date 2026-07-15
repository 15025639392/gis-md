#pragma once

#include "TileSelectionTraversalContext.h"

namespace earth_engine {

class TileContentAccess;
class Tileset;
class TileIncrementalFrontier;
enum class TileOcclusionState;

struct TileSelectionTraversalContextBuildInput {
    TilePlan& tilePlan;
    TileLoadQueue& loadQueue;
    TileSelectionCounters& counters;
    const TilesetOptions& options;
    const std::vector<ActivatedRasterOverlay*>& rasterOverlays;
    RenderDevice* device = nullptr;
    IPrepareRendererResources* pPrepRenderer = nullptr;
    FrameResourceBudget& frameResourceBudget;
    Vec3 lastCameraPosition = Vec3::zero();
    TileContentAccess& contentAccess;
    TileSelectionPerformanceTimings* performanceTimings = nullptr;
    // ③ 增量缓存(nullptr = 全量/影子路径,捕获全 no-op)。
    TileIncrementalFrontier* incremental = nullptr;
};

// The injected policies for a traversal: occlusion (software occlusion vs.
// none) and onVisitTile (live tileset active-set registration vs. shadow-tree
// reset+accumulate). The plain per-frame data lives in the BuildInput above.
struct TileSelectionTraversalContextBinding {
    using CheckOcclusionFn =
        TileOcclusionState (*)(void*, const TilesetTile&);
    using OnVisitTileFn = void (*)(void*, TilesetTile&);

    void* occlusionUserData = nullptr;
    CheckOcclusionFn checkOcclusion = nullptr;
    OnVisitTileFn onVisitTile = nullptr;
    void* onVisitTileUserData = nullptr;
};

class TileSelectionTraversalContextBuilder {
public:
    static TileSelectionTraversalContext build(
        TileSelectionTraversalContextBuildInput input,
        TileSelectionTraversalContextBinding& binding);
};

} // namespace earth_engine
