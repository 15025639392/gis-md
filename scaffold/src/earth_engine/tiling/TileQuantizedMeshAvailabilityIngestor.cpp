#include "TileQuantizedMeshAvailabilityIngestor.h"

#include "../providers/QuantizedMeshTerrainProvider.h"
#include "../terrain/QuantizedMeshParser.h"
#include "../terrain/TerrainTile.h"

#include <array>
#include <vector>

namespace earth_engine {

void TileQuantizedMeshAvailabilityIngestor::ingest(
    TerrainProvider* terrainProvider,
    const TileKey& key,
    DecodedHeightmap& heightmap) {
    if (heightmap.metadataAvailabilityProcessed || heightmap.rawData.empty()) {
        return;
    }
    heightmap.metadataAvailabilityProcessed = true;

    auto* qmProvider =
        dynamic_cast<QuantizedMeshTerrainProvider*>(terrainProvider);
    if (!qmProvider) {
        return;
    }

    if (!qmProvider->isAvailabilityBoundaryLevel(key.z)) {
        return;
    }

    const std::vector<std::array<int, 5>> metadataAvailability =
        QuantizedMeshParser::parseMetadataAvailability(
            heightmap.rawData.data(),
            heightmap.rawData.size());

    for (const auto& r : metadataAvailability) {
        int absLevel = key.z + 1 + r[0];
        if (absLevel >= 0) {
            qmProvider->addAvailabilityRectsForTile(
                key,
                absLevel,
                {{r[1], r[2], r[3], r[4]}});
        }
    }

    // cesium-native addRectangleAvailabilityToLayer marks the subtree loaded
    // even when metadata has no availability rectangles.
    qmProvider->markSubtreeLoadedForTile(key);
}

} // namespace earth_engine
