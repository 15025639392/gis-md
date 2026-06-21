#pragma once

#include "../core/math/Rectangle.h"
#include "../providers/TerrainProvider.h"
#include "../tiling/TileKey.h"

#include <cstddef>
#include <cstdint>
#include <memory>
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
    std::unique_ptr<SurfaceTileMesh> surfaceMesh;
    std::vector<QuantizedMeshAvailabilityUpdate> availabilityUpdates;

    bool success() const {
        return status == QuantizedMeshContentLoadStatus::Success &&
               surfaceMesh != nullptr;
    }
};

class QuantizedMeshContentLoader final {
public:
    static QuantizedMeshContentLoadResult load(
        const uint8_t* data,
        size_t size,
        const Rectangle& tileRectangle,
        bool enableWaterMask,
        const std::vector<QuantizedMeshMetadataContent>& metadata);
};

} // namespace earth_engine
