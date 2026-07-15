#pragma once

#include "TileRasterOverlayState.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace earth_engine {

class ActivatedRasterOverlay;
class FrameResourceBudget;
class RenderDevice;
class Renderer;
struct TilesetTile;

using RenderContentRasterOverlayUpdateAction =
    TileRasterOverlayUpdateAction;

class RenderContentRasterOverlayStateUpdater {
public:
    static RenderContentRasterOverlayUpdateAction update(
        Renderer& renderer,
        TilesetTile& tile,
        const std::vector<ActivatedRasterOverlay*>& rasterOverlays,
        const std::vector<size_t>& overlayProcessingOrder,
        RenderDevice* device,
        double maximumScreenSpaceError,
        FrameResourceBudget& frameResourceBudget,
        uint64_t frameNumber = 0);
};

} // namespace earth_engine
