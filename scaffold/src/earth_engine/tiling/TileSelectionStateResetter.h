#pragma once

#include <vector>

namespace earth_engine {

class ActivatedRasterOverlay;
class TilesetTileRegistry;

class TileSelectionStateResetter {
public:
    static void reset(TilesetTileRegistry& tileRegistry,
                      const std::vector<ActivatedRasterOverlay*>&
                          rasterOverlays);
};

} // namespace earth_engine
