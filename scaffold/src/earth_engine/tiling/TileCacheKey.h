#pragma once

#include "TileKey.h"

#include <string>

namespace earth_engine {

struct TileCacheKey {
    static std::string forTile(const TileKey& key) {
        return key.schemeId.str() + "/" + std::to_string(key.z) + "/" +
               std::to_string(key.x) + "/" + std::to_string(key.y);
    }
};

} // namespace earth_engine
