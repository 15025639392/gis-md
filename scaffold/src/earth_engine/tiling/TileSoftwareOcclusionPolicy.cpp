#include "TileSoftwareOcclusionPolicy.h"

#include "RasterMappedToTilesetTile.h"
#include "TileBoundsMetrics.h"
#include "TilesetTile.h"

#include "../core/geodesy/Cartographic.h"
#include "../core/geodesy/Ellipsoid.h"

#include <algorithm>
#include <cmath>
#include <optional>

namespace earth_engine {
namespace {

constexpr double kPiForLongitudeWrap = 3.14159265358979323846264338327950288;
constexpr double kTwoPiForLongitudeWrap = kPiForLongitudeWrap * 2.0;

// A genuine quantized-mesh horizon occlusion point is expressed in
// ellipsoid-scaled space and therefore has magnitude near 1. The (0,0,0)
// sentinel written by servers that do not compute the point is not usable:
// it maps to the ellipsoid center and would report every tile occluded.
bool isUsableHorizonOcclusionPoint(const Vec3& scaledPoint) {
    constexpr double kMinUsableMagnitudeSquared = 1e-12;
    return scaledPoint.lengthSquared() > kMinUsableMagnitudeSquared;
}

bool scaledPointOccludedByHorizon(const Vec3& pointScaled,
                                  const Vec3& cameraScaled,
                                  double vhMagnitudeSquared) {
    const Vec3 vt = pointScaled - cameraScaled;
    const double vtMagnitudeSquared = vt.lengthSquared();
    if (vtMagnitudeSquared <= 0.0) {
        return false;
    }

    const double vtDotVc = -vt.dot(cameraScaled);
    return vtDotVc > vhMagnitudeSquared &&
           (vtDotVc * vtDotVc) / vtMagnitudeSquared >
               vhMagnitudeSquared;
}

bool pointOccludedByEllipsoid(const Vec3& pointEcef,
                              const Vec3& cameraPosition,
                              const Ellipsoid& ellipsoid) {
    const Vec3 ray = pointEcef - cameraPosition;
    const double distance = ray.length();
    if (distance <= 1e-6) {
        return false;
    }

    const Vec3 direction = ray / distance;
    const auto interval =
        ellipsoid.rayIntersectionInterval(cameraPosition, direction);
    if (!interval) {
        return false;
    }

    return interval->entryDistance > 0.0 &&
           interval->entryDistance < distance - 1.0;
}

bool sphereFullyOccluded(const TileBoundingVolume& volume,
                         const Vec3& cameraPosition,
                         const Ellipsoid& ellipsoid) {
    const Vec3 center = volume.sphere.getCenter();
    const double radius = volume.sphere.getRadius();
    const Vec3 samples[7] = {
        center,
        center + Vec3(radius, 0.0, 0.0),
        center - Vec3(radius, 0.0, 0.0),
        center + Vec3(0.0, radius, 0.0),
        center - Vec3(0.0, radius, 0.0),
        center + Vec3(0.0, 0.0, radius),
        center - Vec3(0.0, 0.0, radius)
    };

    for (const Vec3& sample : samples) {
        if (!pointOccludedByEllipsoid(sample, cameraPosition, ellipsoid)) {
            return false;
        }
    }
    return true;
}

bool boxFullyOccluded(const TileBoundingVolume& volume,
                      const Vec3& cameraPosition,
                      const Ellipsoid& ellipsoid) {
    const Vec3 center = volume.box.getCenter();
    const Vec3 axes[3] = {
        volume.box.getHalfAxis(0),
        volume.box.getHalfAxis(1),
        volume.box.getHalfAxis(2)
    };

    if (!pointOccludedByEllipsoid(center, cameraPosition, ellipsoid)) {
        return false;
    }

    for (int sx : {-1, 1}) {
        for (int sy : {-1, 1}) {
            for (int sz : {-1, 1}) {
                const Vec3 sample =
                    center +
                    static_cast<double>(sx) * axes[0] +
                    static_cast<double>(sy) * axes[1] +
                    static_cast<double>(sz) * axes[2];
                if (!pointOccludedByEllipsoid(sample,
                                              cameraPosition,
                                              ellipsoid)) {
                    return false;
                }
            }
        }
    }
    return true;
}

double tileMidLongitude(const Rectangle& bounds) {
    if (!bounds.crossesAntimeridian()) {
        return (bounds.west() + bounds.east()) * 0.5;
    }

    return std::fmod(bounds.west() + bounds.width() * 0.5 +
                        kPiForLongitudeWrap,
                    kTwoPiForLongitudeWrap) -
           kPiForLongitudeWrap;
}

Rectangle occlusionRectangleForTile(const TilesetTile& tile) {
    if (tile.boundingVolume) {
        const std::optional<Rectangle> estimated =
            tile.boundingVolume->estimateGlobeRectangle(Ellipsoid::WGS84());
        if (estimated) {
            return *estimated;
        }
    }
    return tile.bounds;
}

double occlusionSampleHeightForTile(const TilesetTile& tile) {
    if (!tile.boundingVolume) {
        return std::max(0.0, TileBoundsMetrics::terrainMaximumHeight(tile));
    }

    switch (tile.boundingVolume->kind) {
        case TileBoundingVolumeKind::Region:
            return tile.boundingVolume->maximumHeight;
        case TileBoundingVolumeKind::S2Cell:
            return tile.boundingVolume->s2Cell.getMaximumHeight();
        case TileBoundingVolumeKind::Sphere:
        case TileBoundingVolumeKind::Box:
        case TileBoundingVolumeKind::CylinderRegion:
            break;
    }

    return std::max(0.0, TileBoundsMetrics::terrainMaximumHeight(tile));
}

} // namespace

TileOcclusionState TileSoftwareOcclusionPolicy::check(
    const TilesetTile& tile,
    const Vec3& cameraPosition) {
    const auto& ellipsoid = Ellipsoid::WGS84();
    const Rectangle occlusionRectangle = occlusionRectangleForTile(tile);
    const Cartographic cameraCart =
        ellipsoid.cartesianToCartographic(cameraPosition);
    const bool cameraInsideTileRegion =
        occlusionRectangle.contains(cameraCart.longitude(),
                                    cameraCart.latitude());
    if (cameraCart.height() <= 0.0 ||
        cameraInsideTileRegion) {
        return TileOcclusionState::NotOccluded;
    }

    const Vec3 cameraScaled(
        cameraPosition.x() / ellipsoid.semiMajorAxis(),
        cameraPosition.y() / ellipsoid.semiMajorAxis(),
        cameraPosition.z() / ellipsoid.semiMinorAxis());
    const double vhMagnitudeSquared = cameraScaled.lengthSquared() - 1.0;
    if (vhMagnitudeSquared <= 0.0) {
        return TileOcclusionState::NotOccluded;
    }

    if (const Vec3* horizonOcclusionPoint =
            tile.content.renderContent.horizonOcclusionPoint();
        horizonOcclusionPoint &&
        isUsableHorizonOcclusionPoint(*horizonOcclusionPoint)) {
        // A valid quantized-mesh horizon occlusion point lives in
        // ellipsoid-scaled space (magnitude ~1). Some terrain servers omit it
        // and write the (0,0,0) sentinel; that scaled point is the ellipsoid
        // center, which is trivially behind the horizon, so it would wrongly
        // occlude every visible tile. Skip the fast horizon test for a
        // degenerate point and fall through to the geometric ellipsoid tests
        // below.
        return scaledPointOccludedByHorizon(*horizonOcclusionPoint,
                                            cameraScaled,
                                            vhMagnitudeSquared)
            ? TileOcclusionState::Occluded
            : TileOcclusionState::NotOccluded;
    }

    if (tile.boundingVolume) {
        if (tile.boundingVolume->kind == TileBoundingVolumeKind::Sphere) {
            return sphereFullyOccluded(*tile.boundingVolume,
                                       cameraPosition,
                                       ellipsoid)
                ? TileOcclusionState::Occluded
                : TileOcclusionState::NotOccluded;
        }
        if (tile.boundingVolume->kind == TileBoundingVolumeKind::Box) {
            return boxFullyOccluded(*tile.boundingVolume,
                                    cameraPosition,
                                    ellipsoid)
                ? TileOcclusionState::Occluded
                : TileOcclusionState::NotOccluded;
        }
    }

    const double sampleHeight = occlusionSampleHeightForTile(tile);
    const double midLon = tileMidLongitude(occlusionRectangle);
    const double midLat =
        (occlusionRectangle.south() + occlusionRectangle.north()) * 0.5;
    const double longitudes[3] = {
        occlusionRectangle.west(),
        midLon,
        occlusionRectangle.east()
    };
    const double latitudes[3] = {
        occlusionRectangle.south(),
        midLat,
        occlusionRectangle.north()
    };

    for (double lat : latitudes) {
        for (double lon : longitudes) {
            const Vec3 point = ellipsoid.cartographicToCartesian(
                Cartographic::fromRadians(lon, lat, sampleHeight));
            if (!pointOccludedByEllipsoid(point, cameraPosition, ellipsoid)) {
                return TileOcclusionState::NotOccluded;
            }
        }
    }

    return TileOcclusionState::Occluded;
}

} // namespace earth_engine
