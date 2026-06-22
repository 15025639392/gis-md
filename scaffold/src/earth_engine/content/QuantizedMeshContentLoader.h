#pragma once

#include "GltfModel.h"
#include "../core/math/Rectangle.h"
#include "../terrain/QuantizedMeshAvailability.h"
#include "../tiling/TileBoundingVolume.h"
#include "../tiling/TileKey.h"
#include "../tiling/TileLoadResultMetadata.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace earth_engine {

enum class QuantizedMeshContentLoadStatus {
    Success,
    Failed
};

struct QuantizedMeshMetadataContent {
    int layerIndex = -1;
    TileKey subtreeKey;
    const uint8_t* data = nullptr;
    size_t size = 0;
};

struct QuantizedMeshContentLoadResult {
    QuantizedMeshContentLoadStatus status =
        QuantizedMeshContentLoadStatus::Failed;
    std::unique_ptr<GltfModel> gltfModel;
    TileLoadResultMetadata metadata;
    std::vector<QuantizedMeshAvailabilityUpdate> availabilityUpdates;
    std::vector<std::string> diagnostics;

    bool success() const {
        return status == QuantizedMeshContentLoadStatus::Success &&
               gltfModel != nullptr;
    }
};

class QuantizedMeshContentLoader final {
public:
    static QuantizedMeshContentLoadResult load(
        const uint8_t* data,
        size_t size,
        const Rectangle& tileRectangle,
        bool enableWaterMask,
        const std::vector<QuantizedMeshMetadataContent>& metadata,
        RasterOverlayProjection terrainProjection =
            RasterOverlayProjection::Geographic,
        std::optional<QuantizedMeshAvailabilityUpdate>
            currentTileAvailabilityUpdate = std::nullopt);
};

} // namespace earth_engine
