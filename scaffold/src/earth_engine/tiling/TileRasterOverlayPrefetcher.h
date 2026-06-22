#pragma once

#include <cstddef>
#include <vector>

namespace earth_engine {

class ActivatedRasterOverlay;
class FrameResourceBudget;
class IPrepareRendererResources;
class RenderDevice;
struct TilesetTile;

class TileRasterOverlayPrefetcher {
public:
    static void prefetch(
        TilesetTile& tile,
        const std::vector<ActivatedRasterOverlay*>& rasterOverlays,
        const std::vector<size_t>& overlayProcessingOrder,
        RenderDevice* device,
        double maximumScreenSpaceError,
        FrameResourceBudget& frameResourceBudget,
        IPrepareRendererResources* pPrepRenderer = nullptr);
};

} // namespace earth_engine
