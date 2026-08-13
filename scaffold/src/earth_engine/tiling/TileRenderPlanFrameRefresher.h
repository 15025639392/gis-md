#pragma once

#include <vector>

namespace earth_engine {

class TileContentAccess;
class ActivatedRasterOverlay;
struct TilePlan;

struct TileRenderPlanFrameRefreshOptions {
    bool interactionActive = false;
    bool resourceSmoothingActive = false;
    // Tileset SSE threshold, used to derive the per-tile distance-continuous
    // geomorph factor. 0 disables the derivation (falls back to no morph).
    double maximumScreenSpaceError = 0.0;
    // 接边错位诊断探针(SeamDiag)。默认关:常开每帧 measure 约 4ms(selPlan 大头)。
    bool seamEdgeMismatchProbe = false;
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
