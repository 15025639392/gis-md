#include "TileSelectionRootPolicy.h"

namespace earth_engine {

TileKey TileSelectionRootPolicy::virtualTerrainRootKey(
    const std::string& schemeId) {
    return TileKey{schemeId, -1, 0, 0};
}

bool TileSelectionRootPolicy::isVirtualTerrainRoot(const TileKey& key) {
    return key.z == -1 && key.x == 0 && key.y == 0 &&
           usesVirtualTerrainRoot(key.schemeId);
}

bool TileSelectionRootPolicy::usesVirtualTerrainRoot(
    const std::string& schemeId) {
    return schemeId == "Geographic-TMS" || schemeId == "XYZ-WebMercator";
}

std::vector<TileKey> TileSelectionRootPolicy::levelZeroTerrainRoots(
    const std::string& schemeId) {
    if (schemeId == "Geographic-TMS") {
        return {
            TileKey{schemeId, 0, 0, 0},
            TileKey{schemeId, 0, 1, 0},
        };
    }

    return {TileKey{schemeId, 0, 0, 0}};
}

std::vector<TileKey> TileSelectionRootPolicy::chooseRoots(
    const std::string& schemeId,
    const std::vector<TileKey>& explicitRoots,
    bool useVirtualTerrainRoot) {
    if (!explicitRoots.empty()) {
        // cesium-native TilesetJsonLoader supplies explicit roots from the
        // loaded tileset.json rather than deriving roots from a quadtree scheme.
        return explicitRoots;
    }

    if (useVirtualTerrainRoot && usesVirtualTerrainRoot(schemeId)) {
        return {virtualTerrainRootKey(schemeId)};
    }

    if (schemeId == "Geographic-TMS") {
        return levelZeroTerrainRoots(schemeId);
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
