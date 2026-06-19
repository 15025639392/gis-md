#pragma once

#include "TileLoadDiagnostics.h"

#include <cstdint>

namespace earth_engine {

class Tileset;

class TilesetQueryFacade {
public:
    static uint32_t maximumTransportActiveRequests(const Tileset& tileset);
    static TilesetLoadDiagnostics loadDiagnostics(const Tileset& tileset);
    static float sampleHeight(const Tileset& tileset,
                              double lngRad,
                              double latRad);
};

} // namespace earth_engine
