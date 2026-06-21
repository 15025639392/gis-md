#pragma once

#include "SurfaceTile.h"
#include "TileBoundingVolume.h"

#include <optional>

namespace earth_engine {

/// cesium-native TileLoadResult success metadata shared by native terrain and
/// glTF content loading paths.
struct TileLoadResultMetadata {
    std::optional<TileBoundingVolume> updatedBoundingVolume;
    std::optional<TileBoundingVolume> updatedContentBoundingVolume;
    std::optional<RasterOverlayDetails> rasterOverlayDetails;
};

} // namespace earth_engine
