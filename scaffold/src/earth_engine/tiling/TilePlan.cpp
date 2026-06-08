#include "TilePlan.h"
#include "TileQuadTree.h"

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <cmath>
#include <algorithm>

namespace earth_engine {

namespace {
constexpr double kEarthCircumference = 2.0 * glm::pi<double>() * 6378137.0;

int openGlobusGroupBaseY(int group, int tilesAtZoom) {
    return group * tilesAtZoom;
}

int openGlobusGroupForY(int y, int tilesAtZoom) {
    if (y >= 2 * tilesAtZoom) return 2;
    if (y >= tilesAtZoom) return 1;
    return 0;
}
} // namespace

TilePlan TilePlanBuilder::compute(const Camera& camera,
                                   const TileScheme& scheme,
                                   double viewportWidthPixels,
                                   double viewportHeightPixels,
                                   int previousZoom) {
    TileQuadTree tree;
    return tree.compute(camera, scheme, viewportWidthPixels, viewportHeightPixels, previousZoom);
}

int TilePlanBuilder::zoomLevelFromHeight(double cameraHeightMeters,
                                          double viewportHeightPixels,
                                          double verticalFovRadians,
                                          int tileSize,
                                          int minZoom,
                                          int maxZoom,
                                          int previousZoom) {
    if (viewportHeightPixels <= 0.0) return minZoom;

    // 地面分辨率（米/像素）≈ 相机高度 × 2 × tan(fov/2) / 视口高度
    double metersPerPixel = cameraHeightMeters * 2.0 *
                            std::tan(verticalFovRadians * 0.5) /
                            viewportHeightPixels;

    // 给定 zoom 的地面分辨率 = 地球周长 / (tileSize * 2^zoom)
    // 求解 zoom: 2^zoom = 地球周长 / (tileSize * metersPerPixel)
    double idealTiles = kEarthCircumference / (static_cast<double>(tileSize) * metersPerPixel);
    double idealZoom = std::log2(idealTiles);

    constexpr double kHysteresis = 0.15;
    if (previousZoom >= minZoom && previousZoom <= maxZoom) {
        if (idealZoom > static_cast<double>(previousZoom) - 0.5 + kHysteresis &&
            idealZoom < static_cast<double>(previousZoom) + 0.5 - kHysteresis) {
            return previousZoom;
        }
    }
    int zoom = static_cast<int>(std::round(idealZoom));

    return std::clamp(zoom, minZoom, maxZoom);
}

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
