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
