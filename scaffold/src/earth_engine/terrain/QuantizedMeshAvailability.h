#pragma once

#include "../tiling/SurfaceTile.h"
#include "../tiling/TileKey.h"

#include <vector>

namespace earth_engine {

enum class TileAvailabilityState {
    NotAvailable,
    Available,
    Unknown
};

/// cesium-native LayerJsonTerrainLoader: when loading a tile from an
/// upper layer, availability metadata for matching underlying layers is
/// loaded at the same time. These updates are applied on the main thread.
struct QuantizedMeshAvailabilityUpdate {
    int layerIndex = -1;
    TileKey subtreeKey;
    std::vector<QuantizedMeshAvailabilityRange> metadataAvailability;
};

} // namespace earth_engine
