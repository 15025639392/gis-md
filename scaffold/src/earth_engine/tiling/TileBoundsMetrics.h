#pragma once

#include "TileBoundingVolume.h"
#include "../core/math/OrientedBoundingBox.h"
#include "../core/math/Rectangle.h"
#include "../core/math/Vec3.h"

#include <optional>

namespace earth_engine {

class Frustum;
struct TilesetTile;

/// Cesium-native-aligned bounds metrics used by tile selection, culling,
/// fog distance, occlusion, and viewer-request-volume checks.
class TileBoundsMetrics {
public:
    static constexpr double kDefaultTerrainMinimumHeight = -1000.0;
    static constexpr double kDefaultTerrainMaximumHeight = 9000.0;

    static double terrainMinimumHeight(const TilesetTile& tile);
    static double terrainMaximumHeight(const TilesetTile& tile);

    static Vec3 tileBoundsCenter(const Rectangle& bounds);
    static double tileBoundsRadius(const TilesetTile& tile,
                                   const Vec3& center);
    static std::optional<OrientedBoundingBox> tileBoundingRegionObb(
        const TilesetTile& tile);
    static bool tileIntersectsFrustum(const TilesetTile& tile,
                                      const Frustum& frustum);
    static double approximateDistanceToTileBounds(
        const TilesetTile& tile,
        const Vec3& cameraPosition);

    static Vec3 boundingVolumeCenter(const TileBoundingVolume& volume);
    static double boundingVolumeDistance(const TileBoundingVolume& volume,
                                         const Vec3& cameraPosition);
    static bool boundingVolumeContainsPosition(
        const TileBoundingVolume& volume,
        const Vec3& position);
    static bool boundingVolumeIntersectsFrustum(
        const TileBoundingVolume& volume,
        const Frustum& frustum);
};

} // namespace earth_engine
