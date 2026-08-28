#pragma once

#include "TileTraversalDetails.h"

#include <vector>

namespace earth_engine {

class ActivatedRasterOverlay;
class RasterOverlayFrameContext;
struct TilesetTile;

class TileSelectionTraversalDetailsBuilder {
public:
    static TileTraversalDetails forSingleTile(
        const TilesetTile& tile,
        const RasterOverlayFrameContext& frame);

    static TileTraversalDetails forCulledTile(
        const TilesetTile& tile,
        bool forbidHoles,
        const RasterOverlayFrameContext& frame);
};

} // namespace earth_engine
