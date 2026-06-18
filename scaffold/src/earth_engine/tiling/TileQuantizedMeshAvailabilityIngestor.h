#pragma once

#include "TileKey.h"

namespace earth_engine {

class TerrainProvider;
struct DecodedHeightmap;

class TileQuantizedMeshAvailabilityIngestor {
public:
    static void ingest(TerrainProvider* terrainProvider,
                       const TileKey& key,
                       DecodedHeightmap& heightmap);
};

} // namespace earth_engine
