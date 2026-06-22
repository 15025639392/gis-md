#pragma once

#include <cstddef>
#include <vector>

namespace earth_engine {

class ActivatedRasterOverlay;
class FrameResourceBudget;
class RenderDevice;
class Renderer;
struct TilesetTile;

struct RenderContentRasterOverlayUpdateAction {
    bool unloadTileContent = false;
    bool createRasterOverlayUpsampledChildren = false;
};

class RenderContentRasterOverlayStateUpdater {
public:
    static RenderContentRasterOverlayUpdateAction update(
        Renderer& renderer,
        TilesetTile& tile,
        const std::vector<ActivatedRasterOverlay*>& rasterOverlays,
        const std::vector<size_t>& overlayProcessingOrder,
        RenderDevice* device,
        double maximumScreenSpaceError,
        FrameResourceBudget& frameResourceBudget);
};

} // namespace earth_engine
