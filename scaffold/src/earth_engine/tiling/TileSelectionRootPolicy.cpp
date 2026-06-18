#include "TileSelectionRootPolicy.h"

namespace earth_engine {

std::vector<TileKey> TileSelectionRootPolicy::chooseRoots(
    const std::string& schemeId,
    const std::vector<TileKey>& explicitRoots) {
    if (!explicitRoots.empty()) {
        // cesium-native TilesetJsonLoader supplies explicit roots from the
        // loaded tileset.json rather than deriving roots from a quadtree scheme.
        return explicitRoots;
    }

    if (schemeId == "Geographic-TMS") {
        return {
            TileKey{schemeId, 0, 0, 0},
            TileKey{schemeId, 0, 1, 0},
        };
    }

    if (schemeId == "OpenGlobus-Earth") {
        return {
            TileKey{schemeId, 0, 0, 0},
            TileKey{schemeId, 0, 0, 1},
            TileKey{schemeId, 0, 0, 2},
        };
    }

    return {TileKey{schemeId, 0, 0, 0}};
}

} // namespace earth_engine
