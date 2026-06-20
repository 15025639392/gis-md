#include "TileBoundsMetrics.h"

#include "RasterMappedToTilesetTile.h"
#include "TilesetTile.h"
#include "../core/geodesy/Cartographic.h"
#include "../core/geodesy/Ellipsoid.h"
#include "../core/geodesy/Transforms.h"
#include "../core/math/Plane.h"
#include "../scene/Frustum.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace earth_engine {
namespace {

double terrainHeightPadding(double minimumHeight, double maximumHeight) {
    return std::max(std::abs(minimumHeight), std::abs(maximumHeight));
}

double terrainHeightPadding(const TilesetTile& tile) {
    if (!tile.content.renderContent.hasTerrainHeightRange()) {
        return terrainHeightPadding(
            TileBoundsMetrics::kDefaultTerrainMinimumHeight,
            TileBoundsMetrics::kDefaultTerrainMaximumHeight);
    }
    return terrainHeightPadding(
        tile.content.renderContent.terrainMinimumHeight(),
        tile.content.renderContent.terrainMaximumHeight());
}

bool normalizedOrInvalid(Vec3& value) {
    const double length = value.length();
    if (!std::isfinite(length) || length < 1e-12) {
        return false;
    }
    value = value / length;
    return true;
}

bool intersectRayPlane(const Vec3& rayOrigin,
                       const Vec3& rayDirection,
                       const Vec3& planePoint,
                       const Vec3& planeNormal,
                       Vec3& outPoint) {
    const double denominator = planeNormal.dot(rayDirection);
    if (std::abs(denominator) < 1e-12) {
        return false;
    }
    const double t = -planeNormal.dot(rayOrigin - planePoint) / denominator;
    outPoint = rayOrigin + rayDirection * t;
    return std::isfinite(outPoint.x()) &&
           std::isfinite(outPoint.y()) &&
           std::isfinite(outPoint.z());
}

struct BoundingRegionPlanes {
    Vec3 southwestCornerCartesian = Vec3::zero();
    Vec3 northeastCornerCartesian = Vec3::zero();
    Vec3 westNormal = Vec3::zero();
    Vec3 eastNormal = Vec3::zero();
    Vec3 southNormal = Vec3::zero();
    Vec3 northNormal = Vec3::zero();
    bool valid = false;
};

BoundingRegionPlanes computeBoundingRegionPlanes(
    const Rectangle& bounds,
    const Ellipsoid& ellipsoid) {
    BoundingRegionPlanes planes;
    planes.southwestCornerCartesian = ellipsoid.cartographicToCartesian(
        Cartographic::fromRadians(bounds.west(), bounds.south(), 0.0));
    planes.northeastCornerCartesian = ellipsoid.cartographicToCartesian(
        Cartographic::fromRadians(bounds.east(), bounds.north(), 0.0));

    const double latitudeMidpoint = (bounds.south() + bounds.north()) * 0.5;
    const Vec3 westernMidpointCartesian = ellipsoid.cartographicToCartesian(
        Cartographic::fromRadians(bounds.west(), latitudeMidpoint, 0.0));
    const Vec3 easternMidpointCartesian = ellipsoid.cartographicToCartesian(
        Cartographic::fromRadians(bounds.east(), latitudeMidpoint, 0.0));

    planes.westNormal = westernMidpointCartesian.cross(Vec3::unitZ());
    planes.eastNormal = Vec3::unitZ().cross(easternMidpointCartesian);
    if (!normalizedOrInvalid(planes.westNormal) ||
        !normalizedOrInvalid(planes.eastNormal)) {
        return planes;
    }

    const Vec3 westVector = westernMidpointCartesian - easternMidpointCartesian;
    Vec3 eastWestNormal = westVector;
    if (!normalizedOrInvalid(eastWestNormal)) {
        return planes;
    }

    Vec3 southSurfaceNormal;
    if (bounds.south() > 0.0) {
        const Vec3 southCenterCartesian = ellipsoid.cartographicToCartesian(
            Cartographic::fromRadians(
                (bounds.west() + bounds.east()) * 0.5,
                bounds.south(),
                0.0));
        if (!intersectRayPlane(
                southCenterCartesian,
                eastWestNormal,
                planes.southwestCornerCartesian,
                planes.westNormal,
                planes.southwestCornerCartesian)) {
            return planes;
        }
        southSurfaceNormal =
            ellipsoid.geodeticSurfaceNormal(southCenterCartesian);
    } else {
        southSurfaceNormal = ellipsoid.geodeticSurfaceNormal(
            Cartographic::fromRadians(bounds.east(), bounds.south(), 0.0));
    }
    planes.southNormal = southSurfaceNormal.cross(westVector);
    if (!normalizedOrInvalid(planes.southNormal)) {
        return planes;
    }

    Vec3 northSurfaceNormal;
    if (bounds.north() < 0.0) {
        const Vec3 northCenterCartesian = ellipsoid.cartographicToCartesian(
            Cartographic::fromRadians(
                (bounds.west() + bounds.east()) * 0.5,
                bounds.north(),
                0.0));
        if (!intersectRayPlane(
                northCenterCartesian,
                -eastWestNormal,
                planes.northeastCornerCartesian,
                planes.eastNormal,
                planes.northeastCornerCartesian)) {
            return planes;
        }
        northSurfaceNormal =
            ellipsoid.geodeticSurfaceNormal(northCenterCartesian);
    } else {
        northSurfaceNormal = ellipsoid.geodeticSurfaceNormal(
            Cartographic::fromRadians(bounds.west(), bounds.north(), 0.0));
    }
    planes.northNormal = westVector.cross(northSurfaceNormal);
    if (!normalizedOrInvalid(planes.northNormal)) {
        return planes;
    }

    planes.valid = true;
    return planes;
}

Vec3 tileBoundsCenterFromRectangle(const Rectangle& bounds) {
    const auto& ellipsoid = Ellipsoid::WGS84();
    const double centerLng = (bounds.west() + bounds.east()) * 0.5;
    const double centerLat = (bounds.south() + bounds.north()) * 0.5;
    return ellipsoid.cartographicToCartesian(
        Cartographic::fromRadians(centerLng, centerLat, 0.0));
}

double computeApproximateDistanceToTileBounds(const Rectangle& bounds,
                                              const Vec3& cameraPosition,
                                              double heightPadding) {
    const auto& ellipsoid = Ellipsoid::WGS84();
    const double centerLng = (bounds.west() + bounds.east()) * 0.5;
    const double centerLat = (bounds.south() + bounds.north()) * 0.5;
    const Vec3 center = ellipsoid.cartographicToCartesian(
        Cartographic::fromRadians(centerLng, centerLat, 0.0));

    double radius = 0.0;
    auto expandTo = [&](double lng, double lat) {
        Vec3 p = ellipsoid.cartographicToCartesian(
            Cartographic::fromRadians(lng, lat, 0.0));
        radius = std::max(radius, center.distanceTo(p));
    };
    expandTo(bounds.west(), bounds.south());
    expandTo(bounds.west(), bounds.north());
    expandTo(bounds.east(), bounds.south());
    expandTo(bounds.east(), bounds.north());
    radius += heightPadding;

    return std::max(0.0, cameraPosition.distanceTo(center) - radius);
}

double computeTileBoundsRadius(const Rectangle& bounds,
                               const Vec3& center,
                               double heightPadding) {
    const auto& ellipsoid = Ellipsoid::WGS84();
    double radius = 0.0;
    auto expandTo = [&](double lng, double lat) {
        Vec3 p = ellipsoid.cartographicToCartesian(
            Cartographic::fromRadians(lng, lat, 0.0));
        radius = std::max(radius, center.distanceTo(p));
    };
    expandTo(bounds.west(), bounds.south());
    expandTo(bounds.west(), bounds.north());
    expandTo(bounds.east(), bounds.south());
    expandTo(bounds.east(), bounds.north());
    return radius + heightPadding;
}

Vec3 projectPointToTangentPlane(const Vec3& point,
                                const Vec3& origin,
                                const Vec3& planeNormal) {
    return point - planeNormal * planeNormal.dot(point - origin);
}

double tangentPlaneDistance(const Vec3& point,
                            const Vec3& origin,
                            const Vec3& planeNormal) {
    return planeNormal.dot(point - origin);
}

OrientedBoundingBox obbFromPlaneExtents(
    const Vec3& planeOrigin,
    const Vec3& planeXAxis,
    const Vec3& planeYAxis,
    const Vec3& planeZAxis,
    double minimumX,
    double maximumX,
    double minimumY,
    double maximumY,
    double minimumZ,
    double maximumZ) {
    const Vec3 centerOffset(
        (minimumX + maximumX) * 0.5,
        (minimumY + maximumY) * 0.5,
        (minimumZ + maximumZ) * 0.5);
    const Vec3 scale(
        (maximumX - minimumX) * 0.5,
        (maximumY - minimumY) * 0.5,
        (maximumZ - minimumZ) * 0.5);

    const Vec3 center =
        planeOrigin +
        planeXAxis * centerOffset.x() +
        planeYAxis * centerOffset.y() +
        planeZAxis * centerOffset.z();
    return OrientedBoundingBox(
        center,
        planeXAxis * scale.x(),
        planeYAxis * scale.y(),
        planeZAxis * scale.z());
}

std::optional<OrientedBoundingBox> computeBoundingRegionObb(
    const Rectangle& bounds,
    double minimumHeight,
    double maximumHeight) {
    constexpr double kPi = 3.14159265358979323846264338327950288;
    const auto& ellipsoid = Ellipsoid::WGS84();
    const double centerLongitude = bounds.west() + bounds.width() * 0.5;
    const double centerLatitude = (bounds.south() + bounds.north()) * 0.5;

    if (bounds.width() > kPi) {
        const bool fullyAboveEquator = bounds.south() > 0.0;
        const bool fullyBelowEquator = bounds.north() < 0.0;
        const double latitudeNearestToEquator =
            fullyAboveEquator ? bounds.south()
            : fullyBelowEquator ? bounds.north()
                                : 0.0;

        Vec3 planeOrigin = ellipsoid.cartographicToCartesian(
            Cartographic::fromRadians(
                centerLongitude, latitudeNearestToEquator, maximumHeight));
        planeOrigin.z() = 0.0;

        const bool isPole =
            std::abs(planeOrigin.x()) < 1e-10 &&
            std::abs(planeOrigin.y()) < 1e-10;
        const Vec3 planeNormal = isPole
            ? Vec3::unitX()
            : planeOrigin.normalized();
        const Vec3 planeYAxis = Vec3::unitZ();
        const Vec3 planeXAxis = planeNormal.cross(planeYAxis);
        const Plane plane(planeOrigin, planeNormal);

        const Vec3 horizonCartesian = ellipsoid.cartographicToCartesian(
            Cartographic::fromRadians(
                centerLongitude + kPi * 0.5,
                latitudeNearestToEquator,
                maximumHeight));
        const Vec3 horizonProjected =
            horizonCartesian -
            planeNormal * plane.getPointDistance(horizonCartesian);
        const double maxX = horizonProjected.dot(planeXAxis);
        const double minX = -maxX;

        const double maxY = ellipsoid.cartographicToCartesian(
            Cartographic::fromRadians(
                0.0,
                bounds.north(),
                fullyBelowEquator ? minimumHeight : maximumHeight)).z();
        const double minY = ellipsoid.cartographicToCartesian(
            Cartographic::fromRadians(
                0.0,
                bounds.south(),
                fullyAboveEquator ? minimumHeight : maximumHeight)).z();
        const Vec3 farZ = ellipsoid.cartographicToCartesian(
            Cartographic::fromRadians(
                bounds.east(),
                latitudeNearestToEquator,
                maximumHeight));
        const double minZ = plane.getPointDistance(farZ);
        const double maxZ = 0.0;

        return obbFromPlaneExtents(
            planeOrigin,
            planeXAxis,
            planeYAxis,
            planeNormal,
            minX,
            maxX,
            minY,
            maxY,
            minZ,
            maxZ);
    }

    const Vec3 tangentPoint = ellipsoid.cartographicToCartesian(
        Cartographic::fromRadians(centerLongitude, centerLatitude, 0.0));
    const Vec3 origin = ellipsoid.scaleToGeodeticSurface(tangentPoint);

    const Mat4 tangentFrame =
        Transforms::eastNorthUpToFixedFrame(origin, ellipsoid);
    const Vec3 xAxis(tangentFrame(0, 0),
                     tangentFrame(1, 0),
                     tangentFrame(2, 0));
    const Vec3 yAxis(tangentFrame(0, 1),
                     tangentFrame(1, 1),
                     tangentFrame(2, 1));
    const Vec3 zAxis(tangentFrame(0, 2),
                     tangentFrame(1, 2),
                     tangentFrame(2, 2));

    const double latCenter =
        bounds.south() < 0.0 && bounds.north() > 0.0
            ? 0.0
            : centerLatitude;

    auto toPlane = [&](double lng, double lat, double height) {
        const Vec3 p = ellipsoid.cartographicToCartesian(
            Cartographic::fromRadians(lng, lat, height));
        const Vec3 projected = projectPointToTangentPlane(p, origin, zAxis);
        const Vec3 v = projected - origin;
        return std::array<double, 2>{xAxis.dot(v), yAxis.dot(v)};
    };

    const auto nc = toPlane(centerLongitude, bounds.north(), maximumHeight);
    auto nw = toPlane(bounds.west(), bounds.north(), maximumHeight);
    const auto cw = toPlane(bounds.west(), latCenter, maximumHeight);
    auto sw = toPlane(bounds.west(), bounds.south(), maximumHeight);
    const auto sc = toPlane(centerLongitude, bounds.south(), maximumHeight);

    double minX = std::min({nw[0], cw[0], sw[0]});
    double maxX = -minX;
    double maxY = std::max(nw[1], nc[1]);
    double minY = std::min(sw[1], sc[1]);

    const Vec3 nwMin = ellipsoid.cartographicToCartesian(
        Cartographic::fromRadians(bounds.west(), bounds.north(), minimumHeight));
    const Vec3 swMin = ellipsoid.cartographicToCartesian(
        Cartographic::fromRadians(bounds.west(), bounds.south(), minimumHeight));
    double minZ = std::min(
        tangentPlaneDistance(nwMin, origin, zAxis),
        tangentPlaneDistance(swMin, origin, zAxis));
    double maxZ = maximumHeight;

    constexpr double oneMillimeter = 0.001;
    if (maxX - minX < oneMillimeter) {
        minX -= oneMillimeter * 0.5;
        maxX += oneMillimeter * 0.5;
    }
    if (maxY - minY < oneMillimeter) {
        minY -= oneMillimeter * 0.5;
        maxY += oneMillimeter * 0.5;
    }
    if (maxZ - minZ < oneMillimeter) {
        minZ -= oneMillimeter * 0.5;
        maxZ += oneMillimeter * 0.5;
    }

    return obbFromPlaneExtents(
        origin,
        xAxis,
        yAxis,
        zAxis,
        minX,
        maxX,
        minY,
        maxY,
        minZ,
        maxZ);
}

double computeBoundingRegionDistanceSquared(
    const Rectangle& bounds,
    double minimumHeight,
    double maximumHeight,
    const Vec3& cameraPosition) {
    const auto& ellipsoid = Ellipsoid::WGS84();
    const auto cameraCart =
        ellipsoid.tryCartesianToCartographic(cameraPosition);
    if (!cameraCart) {
        const std::optional<OrientedBoundingBox> obb =
            computeBoundingRegionObb(bounds, minimumHeight, maximumHeight);
        if (obb) {
            return obb->computeDistanceSquaredToPosition(cameraPosition);
        }
        const double distance = computeApproximateDistanceToTileBounds(
            bounds,
            cameraPosition,
            terrainHeightPadding(minimumHeight, maximumHeight));
        return distance * distance;
    }

    const BoundingRegionPlanes planes =
        computeBoundingRegionPlanes(bounds, ellipsoid);
    if (!planes.valid) {
        const std::optional<OrientedBoundingBox> obb =
            computeBoundingRegionObb(bounds, minimumHeight, maximumHeight);
        if (obb) {
            return obb->computeDistanceSquaredToPosition(cameraPosition);
        }
        const double distance = computeApproximateDistanceToTileBounds(
            bounds,
            cameraPosition,
            terrainHeightPadding(minimumHeight, maximumHeight));
        return distance * distance;
    }

    double result = 0.0;
    if (!bounds.contains(cameraCart->longitude(), cameraCart->latitude())) {
        const Vec3 vectorFromSouthwestCorner =
            cameraPosition - planes.southwestCornerCartesian;
        const double distanceToWestPlane =
            vectorFromSouthwestCorner.dot(planes.westNormal);
        const double distanceToSouthPlane =
            vectorFromSouthwestCorner.dot(planes.southNormal);

        const Vec3 vectorFromNortheastCorner =
            cameraPosition - planes.northeastCornerCartesian;
        const double distanceToEastPlane =
            vectorFromNortheastCorner.dot(planes.eastNormal);
        const double distanceToNorthPlane =
            vectorFromNortheastCorner.dot(planes.northNormal);

        if (distanceToWestPlane > 0.0) {
            result += distanceToWestPlane * distanceToWestPlane;
        } else if (distanceToEastPlane > 0.0) {
            result += distanceToEastPlane * distanceToEastPlane;
        }

        if (distanceToSouthPlane > 0.0) {
            result += distanceToSouthPlane * distanceToSouthPlane;
        } else if (distanceToNorthPlane > 0.0) {
            result += distanceToNorthPlane * distanceToNorthPlane;
        }
    }

    const double cameraHeight = cameraCart->height();
    if (cameraHeight > maximumHeight) {
        const double distanceAboveTop = cameraHeight - maximumHeight;
        result += distanceAboveTop * distanceAboveTop;
    } else if (cameraHeight < minimumHeight) {
        const double distanceBelowBottom = minimumHeight - cameraHeight;
        result += distanceBelowBottom * distanceBelowBottom;
    }

    const std::optional<OrientedBoundingBox> obb =
        computeBoundingRegionObb(bounds, minimumHeight, maximumHeight);
    if (obb) {
        result = std::max(
            result,
            obb->computeDistanceSquaredToPosition(cameraPosition));
    }

    return std::max(0.0, result);
}

} // namespace

double TileBoundsMetrics::terrainMinimumHeight(const TilesetTile& tile) {
    return tile.content.renderContent.hasTerrainHeightRange()
        ? tile.content.renderContent.terrainMinimumHeight()
        : kDefaultTerrainMinimumHeight;
}

double TileBoundsMetrics::terrainMaximumHeight(const TilesetTile& tile) {
    return tile.content.renderContent.hasTerrainHeightRange()
        ? tile.content.renderContent.terrainMaximumHeight()
        : kDefaultTerrainMaximumHeight;
}

Vec3 TileBoundsMetrics::tileBoundsCenter(const Rectangle& bounds) {
    return tileBoundsCenterFromRectangle(bounds);
}

double TileBoundsMetrics::tileBoundsRadius(const TilesetTile& tile,
                                           const Vec3& center) {
    if (tile.boundingVolume) {
        switch (tile.boundingVolume->kind) {
            case TileBoundingVolumeKind::Region:
                return computeTileBoundsRadius(
                    tile.boundingVolume->region,
                    center,
                    terrainHeightPadding(
                        tile.boundingVolume->minimumHeight,
                        tile.boundingVolume->maximumHeight));
            case TileBoundingVolumeKind::Sphere:
                return tile.boundingVolume->sphere.getRadius();
            case TileBoundingVolumeKind::Box:
                return tile.boundingVolume->box.toSphere().getRadius();
            case TileBoundingVolumeKind::CylinderRegion:
                return tile.boundingVolume->cylinderRegion
                    .toOrientedBoundingBox()
                    .toSphere()
                    .getRadius();
            case TileBoundingVolumeKind::S2Cell:
                break;
        }
    }
    return computeTileBoundsRadius(
        tile.bounds,
        center,
        terrainHeightPadding(tile));
}

std::optional<OrientedBoundingBox> TileBoundsMetrics::boundingRegionObb(
    const Rectangle& bounds,
    double minimumHeight,
    double maximumHeight) {
    return computeBoundingRegionObb(bounds, minimumHeight, maximumHeight);
}

std::optional<OrientedBoundingBox> TileBoundsMetrics::tileBoundingRegionObb(
    const TilesetTile& tile) {
    if (tile.boundingVolume) {
        if (tile.boundingVolume->kind == TileBoundingVolumeKind::Region) {
            return boundingRegionObb(
                tile.boundingVolume->region,
                tile.boundingVolume->minimumHeight,
                tile.boundingVolume->maximumHeight);
        }
        if (tile.boundingVolume->kind == TileBoundingVolumeKind::Box) {
            return tile.boundingVolume->box;
        }
        if (tile.boundingVolume->kind ==
            TileBoundingVolumeKind::CylinderRegion) {
            return tile.boundingVolume->cylinderRegion.toOrientedBoundingBox();
        }
    }
    return boundingRegionObb(
        tile.bounds,
        terrainMinimumHeight(tile),
        terrainMaximumHeight(tile));
}

bool TileBoundsMetrics::tileIntersectsFrustum(const TilesetTile& tile,
                                              const Frustum& frustum) {
    if (tile.boundingVolume) {
        return boundingVolumeIntersectsFrustum(*tile.boundingVolume, frustum);
    }
    if (const std::optional<OrientedBoundingBox> obb =
            tileBoundingRegionObb(tile)) {
        return frustum.intersectsOBB(*obb);
    }
    const Vec3 center = tileBoundsCenter(tile.bounds);
    return frustum.intersectsSphere(
        center,
        tileBoundsRadius(tile, center));
}

double TileBoundsMetrics::approximateDistanceToTileBounds(
    const TilesetTile& tile,
    const Vec3& cameraPosition) {
    if (tile.boundingVolume) {
        return boundingVolumeDistance(*tile.boundingVolume, cameraPosition);
    }
    const double distanceSquared = computeBoundingRegionDistanceSquared(
        tile.bounds,
        terrainMinimumHeight(tile),
        terrainMaximumHeight(tile),
        cameraPosition);
    return std::sqrt(std::max(0.0, distanceSquared));
}

Vec3 TileBoundsMetrics::boundingVolumeCenter(
    const TileBoundingVolume& volume) {
    switch (volume.kind) {
        case TileBoundingVolumeKind::Region:
            if (const std::optional<OrientedBoundingBox> obb =
                    boundingRegionObb(
                        volume.region,
                        volume.minimumHeight,
                        volume.maximumHeight)) {
                return obb->getCenter();
            }
            return tileBoundsCenterFromRectangle(volume.region);
        case TileBoundingVolumeKind::Sphere:
            return volume.sphere.getCenter();
        case TileBoundingVolumeKind::Box:
            return volume.box.getCenter();
        case TileBoundingVolumeKind::CylinderRegion:
            return volume.cylinderRegion.getCenter();
        case TileBoundingVolumeKind::S2Cell:
            return volume.s2Cell.getCenter();
    }
    return Vec3::zero();
}

double TileBoundsMetrics::boundingVolumeDistance(
    const TileBoundingVolume& volume,
    const Vec3& cameraPosition) {
    switch (volume.kind) {
        case TileBoundingVolumeKind::Region:
            return std::sqrt(std::max(
                0.0,
                computeBoundingRegionDistanceSquared(
                    volume.region,
                    volume.minimumHeight,
                    volume.maximumHeight,
                    cameraPosition)));
        case TileBoundingVolumeKind::Sphere:
            return std::sqrt(std::max(
                0.0,
                volume.sphere.computeDistanceSquaredToPosition(cameraPosition)));
        case TileBoundingVolumeKind::Box:
            return std::sqrt(std::max(
                0.0,
                volume.box.computeDistanceSquaredToPosition(cameraPosition)));
        case TileBoundingVolumeKind::CylinderRegion:
            return std::sqrt(std::max(
                0.0,
                volume.cylinderRegion.computeDistanceSquaredToPosition(
                    cameraPosition)));
        case TileBoundingVolumeKind::S2Cell:
            return std::sqrt(std::max(
                0.0,
                volume.s2Cell.computeDistanceSquaredToPosition(cameraPosition)));
    }
    return 0.0;
}

bool TileBoundsMetrics::boundingVolumeContainsPosition(
    const TileBoundingVolume& volume,
    const Vec3& position) {
    switch (volume.kind) {
        case TileBoundingVolumeKind::Region:
            return computeBoundingRegionDistanceSquared(
                       volume.region,
                       volume.minimumHeight,
                       volume.maximumHeight,
                       position) <= 1e-8;
        case TileBoundingVolumeKind::Sphere:
            return volume.sphere.contains(position);
        case TileBoundingVolumeKind::Box:
            return volume.box.computeDistanceSquaredToPosition(position) <=
                   1e-8;
        case TileBoundingVolumeKind::CylinderRegion:
            return volume.cylinderRegion.contains(position);
        case TileBoundingVolumeKind::S2Cell:
            return volume.s2Cell.contains(position);
    }
    return false;
}

bool TileBoundsMetrics::boundingVolumeIntersectsFrustum(
    const TileBoundingVolume& volume,
    const Frustum& frustum) {
    switch (volume.kind) {
        case TileBoundingVolumeKind::Region: {
            const std::optional<OrientedBoundingBox> obb =
                computeBoundingRegionObb(
                    volume.region,
                    volume.minimumHeight,
                    volume.maximumHeight);
            if (obb) return frustum.intersectsOBB(*obb);
            const Vec3 center = tileBoundsCenterFromRectangle(volume.region);
            const double radius = computeTileBoundsRadius(
                volume.region,
                center,
                terrainHeightPadding(
                    volume.minimumHeight,
                    volume.maximumHeight));
            return frustum.intersectsSphere(center, radius);
        }
        case TileBoundingVolumeKind::Sphere:
            return frustum.intersectsSphere(volume.sphere);
        case TileBoundingVolumeKind::Box:
            return frustum.intersectsOBB(volume.box);
        case TileBoundingVolumeKind::CylinderRegion:
            return frustum.intersectsOBB(
                volume.cylinderRegion.toOrientedBoundingBox());
        case TileBoundingVolumeKind::S2Cell:
            return true;
    }
    return false;
}

} // namespace earth_engine
