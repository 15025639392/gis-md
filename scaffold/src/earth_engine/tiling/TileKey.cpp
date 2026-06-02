#include "TileKey.h"
#include <ostream>

namespace earth_engine {

std::ostream& operator<<(std::ostream& os, const TileKey& key) {
    return os << "TileKey(" << key.schemeId
              << " z=" << key.z
              << " x=" << key.x
              << " y=" << key.y << ")";
}

} // namespace earth_engine
