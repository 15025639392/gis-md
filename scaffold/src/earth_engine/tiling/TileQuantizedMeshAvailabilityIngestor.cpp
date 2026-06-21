#include "TileQuantizedMeshAvailabilityIngestor.h"

#include "../providers/QuantizedMeshTerrainProvider.h"
#include "../terrain/TerrainTile.h"
#include "SurfaceTile.h"

#include <array>
#include <vector>

namespace earth_engine {

void TileQuantizedMeshAvailabilityIngestor::ingest(
    TerrainProvider* terrainProvider,
    const TileKey& key,
    DecodedHeightmap& heightmap,
    const SurfaceTileMesh* surfaceMesh) {
    if (heightmap.metadataAvailabilityProcessed) {
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

    if (!surfaceMesh || !surfaceMesh->hasMetadataAvailability) {
        return;
    }

    for (const auto& r : surfaceMesh->metadataAvailability) {
        int absLevel = key.z + 1 + static_cast<int>(r[0]);
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
