#pragma once

#include <vector>

namespace earth_engine {

class TileContentAccess;
class ActivatedRasterOverlay;
struct TilePlan;

struct TileRenderPlanFrameRefreshOptions {
    bool enableLodTransitionPeriod = false;
    bool interactionActive = false;
    bool resourceSmoothingActive = false;
    // Tileset SSE threshold, used to derive the per-tile distance-continuous
    // geomorph factor. 0 disables the derivation (falls back to no morph).
    double maximumScreenSpaceError = 0.0;
};

class TileRenderPlanFrameRefresher {
public:
    static void refresh(TilePlan& tilePlan,
                        TileContentAccess& contentAccess,
                        const std::vector<ActivatedRasterOverlay*>&
                            rasterOverlays,
                        const TileRenderPlanFrameRefreshOptions& options);
};

} // namespace earth_engine
