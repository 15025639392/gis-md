#pragma once

#include <string>
#include <unordered_set>
#include <vector>

namespace earth_engine {

class ActivatedRasterOverlay;
class TilesetTileRegistry;
struct TilePlan;
struct TilesetTile;

struct TileLodTransitionFrameOptions {
    bool enableLodTransitionPeriod = false;
    double lodTransitionLength = 1.0;
};

class TileLodTransitionFrameUpdater {
public:
    static void update(TilePlan& tilePlan,
                       TilesetTileRegistry& tileRegistry,
                       const std::vector<TilesetTile*>& activeTiles,
                       const std::vector<TilesetTile*>& previousActiveTiles,
                       std::unordered_set<std::string>& fadingKeys,
                       const std::vector<ActivatedRasterOverlay*>&
                           rasterOverlays,
                       double deltaSeconds,
                       const TileLodTransitionFrameOptions& options);
};

} // namespace earth_engine
