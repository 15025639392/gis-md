#include "TilePlan.h"

#include <algorithm>

namespace earth_engine {

namespace {
int openGlobusGroupBaseY(int group, int tilesAtZoom) {
    return group * tilesAtZoom;
}

int openGlobusGroupForY(int y, int tilesAtZoom) {
    if (y >= 2 * tilesAtZoom) return 2;
    if (y >= tilesAtZoom) return 1;
    return 0;
}
} // namespace

TileKey TilePlanBuilder::parentKey(const TileKey& key) {
    if (key.z <= 0) return key;
    if (key.schemeId == "OpenGlobus-Earth") {
        const int childTilesAtZoom = 1 << key.z;
        const int parentTilesAtZoom = 1 << (key.z - 1);
        const int group = openGlobusGroupForY(key.y, childTilesAtZoom);
        const int localY = std::clamp(
            key.y - openGlobusGroupBaseY(group, childTilesAtZoom),
            0,
            childTilesAtZoom - 1);
        return TileKey{
            key.schemeId,
            key.z - 1,
            std::max(0, key.x / 2),
            openGlobusGroupBaseY(group, parentTilesAtZoom) + std::max(0, localY / 2)
        };
    }
    return TileKey{
        key.schemeId,
        key.z - 1,
        std::max(0, key.x / 2),
        std::max(0, key.y / 2)
    };
}

} // namespace earth_engine
