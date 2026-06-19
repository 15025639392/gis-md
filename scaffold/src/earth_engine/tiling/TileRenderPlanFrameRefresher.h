#pragma once

namespace earth_engine {

class TileContentAccess;
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
                        const TileRenderPlanFrameRefreshOptions& options);
};

} // namespace earth_engine
