#pragma once

#include "TileRasterOverlayState.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace earth_engine {

class ActivatedRasterOverlay;
class FrameResourceBudget;
class IPrepareRendererResources;
class RenderDevice;
struct TilesetTile;

using TileRasterOverlayPrefetchAction = TileRasterOverlayUpdateAction;

class TileRasterOverlayPrefetcher {
public:
    static TileRasterOverlayPrefetchAction prefetch(
        TilesetTile& tile,
        const std::vector<ActivatedRasterOverlay*>& rasterOverlays,
        const std::vector<size_t>& overlayProcessingOrder,
        RenderDevice* device,
        double maximumScreenSpaceError,
        FrameResourceBudget& frameResourceBudget,
        IPrepareRendererResources* pPrepRenderer = nullptr,
        uint64_t frameNumber = 0);

    // cesium updateTileOverlays 的每帧廉价路径:对「已映射」的瓦片只推进
    // throttled 影像加载,不重算投影几何、不走 mapped.update 的祖先回退。
    // 完整几何映射是「加载时一次」的动作(cesium addTileOverlays);加载中
    // (not-Done)瓦片每帧只需这个廉价推进,几何在首次(未映射)由 prefetch
    // 算一次。这消除了拖动时对数百个 in-flight 瓦片每帧重算映射的开销。
    static void advanceThrottledLoads(
        TilesetTile& tile,
        const std::vector<ActivatedRasterOverlay*>& rasterOverlays,
        const std::vector<size_t>& overlayProcessingOrder,
        RenderDevice* device,
        FrameResourceBudget& frameResourceBudget);
};

} // namespace earth_engine
