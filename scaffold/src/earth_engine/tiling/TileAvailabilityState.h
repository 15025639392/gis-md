#pragma once

namespace earth_engine {

/// cesium-native LayerJsonTerrainLoader::tileIsAvailableInLayer equivalent.
/// Available means the content can be requested now. Unknown means a metadata
/// availability subtree has not been loaded yet. NotAvailable means the source
/// does not cover this tile.
enum class TileAvailabilityState {
    NotAvailable,
    Available,
    Unknown
};

} // namespace earth_engine
