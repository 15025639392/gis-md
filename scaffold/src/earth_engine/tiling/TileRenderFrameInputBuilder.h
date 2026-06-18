#pragma once

#include "TileRenderFrameBuilder.h"

namespace earth_engine {

struct TileRenderFrameInputBuilder {
    static TileRenderFrameBuildInput build(
        TilePlan& tilePlan,
        const std::unordered_map<
            std::string,
            std::unique_ptr<TilesetTile>>& tiles,
        TileUnloadQueue& unloadQueue,
        std::vector<ActivatedRasterOverlay*>& rasterOverlays,
        bool& cacheBytesDirty,
        uint64_t frameNumber,
        const Vec3& lastCameraPosition,
        const std::vector<FogDensityAtHeight>& fogDensityTable,
        int fogCulled,
        bool resourceSmoothingActive,
        bool interactionActive,
        int64_t totalBytesUsed,
        int64_t maximumCachedBytes) {
        return TileRenderFrameBuildInput{
            tilePlan,
            tiles,
            unloadQueue,
            rasterOverlays,
            cacheBytesDirty,
            frameNumber,
            lastCameraPosition,
            fogDensityTable,
            fogCulled,
            resourceSmoothingActive,
            interactionActive,
            totalBytesUsed,
            maximumCachedBytes};
    }
};

} // namespace earth_engine
