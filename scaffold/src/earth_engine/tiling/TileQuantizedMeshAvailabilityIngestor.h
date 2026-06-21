#pragma once

#include "TileKey.h"

namespace earth_engine {

class TerrainProvider;
struct DecodedHeightmap;
struct SurfaceTileMesh;

class TileQuantizedMeshAvailabilityIngestor {
public:
    static void ingest(TerrainProvider* terrainProvider,
                       const TileKey& key,
                       DecodedHeightmap* heightmap,
                       const SurfaceTileMesh* surfaceMesh);
};

} // namespace earth_engine
