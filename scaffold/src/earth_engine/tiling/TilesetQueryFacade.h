#pragma once

#include "TileLoadDiagnostics.h"

#include <cstdint>

namespace earth_engine {

class Tileset;

class TilesetQueryFacade {
public:
    static int cachedTerrainTiles(const Tileset& tileset);
    static int pendingRequests(const Tileset& tileset);
    static int64_t totalBytesUsed(const Tileset& tileset);
    static uint32_t maximumTransportActiveRequests(const Tileset& tileset);
    static TilesetLoadDiagnostics loadDiagnostics(const Tileset& tileset);
    static float sampleHeight(const Tileset& tileset,
                              double lngRad,
                              double latRad);
};

} // namespace earth_engine
