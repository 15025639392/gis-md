#pragma once

#include <string>
#include <unordered_set>
#include <vector>

namespace earth_engine {

class ActivatedRasterOverlay;
class TilesetTileRegistry;
struct TilePlan;

struct TileLodTransitionFrameOptions {
    bool enableLodTransitionPeriod = false;
    double lodTransitionLength = 1.0;
};

class TileLodTransitionFrameUpdater {
public:
    static void update(TilePlan& tilePlan,
                       TilesetTileRegistry& tileRegistry,
                       std::unordered_set<std::string>& fadingKeys,
                       const std::vector<ActivatedRasterOverlay*>&
                           rasterOverlays,
                       double deltaSeconds,
                       const TileLodTransitionFrameOptions& options);
};

} // namespace earth_engine
