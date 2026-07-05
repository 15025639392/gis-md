#pragma once

#include <vector>

namespace earth_engine {

class ActivatedRasterOverlay;
struct TilesetTile;

class TileSelectionStateResetter {
public:
    // Reset a single tile's per-frame selection state (shift current
    // selectionState → previousSelectionState, clear scratch fields,
    // recompute renderability). Applied incrementally to only the tiles in
    // the active-set, replacing the old per-frame full-registry sweep.
    static void resetOne(TilesetTile& tile,
                         const std::vector<ActivatedRasterOverlay*>&
                             rasterOverlays);
};

} // namespace earth_engine
