#pragma once

#include <cstddef>
#include <vector>

namespace earth_engine {

class ActivatedRasterOverlay;
struct TilesetTile;

class TileRasterOverlayReadinessPolicy {
public:
    static bool requiredOverlaysReady(
        const TilesetTile& tile,
        const std::vector<ActivatedRasterOverlay*>& rasterOverlays);

    static std::vector<size_t> processingOrder(
        const std::vector<ActivatedRasterOverlay*>& rasterOverlays);
};

} // namespace earth_engine
