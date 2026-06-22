#pragma once

#include "GltfModel.h"
#include "../tiling/TileID.h"

#include <memory>

namespace earth_engine {

class GltfTerrainUpsampler {
public:
    static std::unique_ptr<GltfModel> upsampleForRasterOverlay(
        const GltfModel& parentModel,
        const UpsampledQuadtreeNode& childID,
        int textureCoordinateIndex = 0,
        bool hasInvertedVCoordinate = false);
};

} // namespace earth_engine
