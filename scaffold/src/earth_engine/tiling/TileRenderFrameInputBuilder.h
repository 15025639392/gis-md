#pragma once

#include "TileRenderFrameBuilder.h"

namespace earth_engine {

struct TileRenderFrameInputBuilder {
    static TileRenderFrameBuildInput build(
        TilePlan& tilePlan,
        const std::vector<TileRenderEntry>* renderEntriesOverride,
        std::vector<ActivatedRasterOverlay*>& rasterOverlays,
        uint64_t frameNumber,
        const Vec3& lastCameraPosition,
        const std::vector<FogDensityAtHeight>& fogDensityTable,
        int fogCulled,
        bool resourceSmoothingActive,
        bool interactionActive) {
        return TileRenderFrameBuildInput{
            tilePlan,
            rasterOverlays,
            frameNumber,
            lastCameraPosition,
            fogDensityTable,
            fogCulled,
            resourceSmoothingActive,
            interactionActive,
            renderEntriesOverride};
    }
};

} // namespace earth_engine
