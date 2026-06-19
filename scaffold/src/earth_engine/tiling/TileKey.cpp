#include "TileKey.h"
#include <ostream>

namespace earth_engine {

TileKey TileKey::parent() const {
    if (z <= 0) return *this;
    return TileKey{schemeId, z - 1, x >> 1, y >> 1};
}

int TileKey::invertedX(int tilesAtLevelX) const {
    return tilesAtLevelX - x - 1;
}

int TileKey::invertedY(int tilesAtLevelY) const {
    return tilesAtLevelY - y - 1;
}

std::ostream& operator<<(std::ostream& os, const TileKey& key) {
    return os << "TileKey(" << key.schemeId
              << " z=" << key.z
              << " x=" << key.x
              << " y=" << key.y << ")";
}

} // namespace earth_engine
