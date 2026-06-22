#pragma once

#include "../core/math/Vec3.h"
#include "SurfaceTile.h"
#include "TileBoundingVolume.h"

#include <optional>
#include <utility>

namespace earth_engine {

/// cesium-native TileLoadResult success metadata shared by native terrain and
/// glTF content loading paths.
struct TileLoadResultMetadata {
    std::optional<TileBoundingVolume> updatedBoundingVolume;
    std::optional<TileBoundingVolume> updatedContentBoundingVolume;
    std::optional<RasterOverlayDetails> rasterOverlayDetails;
    std::optional<TileBoundingVolume> initialBoundingVolume;
    std::optional<TileBoundingVolume> initialContentBoundingVolume;
    std::optional<std::pair<double, double>> terrainHeightRange;
    std::optional<Vec3> horizonOcclusionPoint;
};

} // namespace earth_engine
