#pragma once

#include "TileTraversalDetails.h"

#include <vector>

namespace earth_engine {

class ActivatedRasterOverlay;
struct TilesetTile;

class TileSelectionTraversalDetailsBuilder {
public:
    static TileTraversalDetails forSingleTile(
        const TilesetTile& tile,
        const std::vector<ActivatedRasterOverlay*>& rasterOverlays);

    static TileTraversalDetails forCulledTile(
        const TilesetTile& tile,
        const std::vector<ActivatedRasterOverlay*>& rasterOverlays,
        bool forbidHoles);
};

} // namespace earth_engine
