#include "Tileset.h"
#include "../scene/FrameState.h"
#include "../scene/Camera.h"
#include "../scene/Frustum.h"
#include "../renderer/Renderer.h"
#include "../renderer/RenderDevice.h"
#include "../core/geodesy/Ellipsoid.h"
#include "../core/geodesy/Cartographic.h"
#include "../core/geodesy/Transforms.h"
#include "../tiling/TileSurface.h"
#include "../tiling/SurfaceRasterBinding.h"
#include "../terrain/QuantizedMeshParser.h"
#include "../providers/QuantizedMeshTerrainProvider.h"
#include "../providers/RasterOverlayTileProvider.h"
#include "../terrain/TerrainTile.h"
#include "../layers/ActivatedRasterOverlay.h"
#include "../layers/RasterOverlay.h"
#include "../debug/PerfTimer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <utility>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_inverse.hpp>

#ifdef __ANDROID__
#include <android/log.h>
#endif

namespace earth_engine {

struct SelectorFrame {
    std::vector<FrameState::SelectorView> views;
    std::vector<double> fogDensities;
};

// ── cesium-native fog implementation ──

double Tileset::computeFogDensity(
    const std::vector<FogDensityAtHeight>& fogDensityTable,
    double cameraHeightMeters) {
    if (fogDensityTable.empty()) {
        return 0.0;
    }

    // cesium-native: binary search for the entry >= camera height
    auto nextIt = std::lower_bound(
        fogDensityTable.begin(), fogDensityTable.end(), cameraHeightMeters,
        [](const FogDensityAtHeight& entry, double height) {
            return entry.cameraHeight < height;
        });

    if (nextIt == fogDensityTable.end()) {
        return fogDensityTable.back().fogDensity;
    }
    if (nextIt == fogDensityTable.begin()) {
        return nextIt->fogDensity;
    }

    auto prevIt = nextIt - 1;
    double t = (cameraHeightMeters - prevIt->cameraHeight) /
               (nextIt->cameraHeight - prevIt->cameraHeight);
    t = std::max(0.0, std::min(1.0, t));

    return prevIt->fogDensity + t * (nextIt->fogDensity - prevIt->fogDensity);
}

bool Tileset::isVisibleInFog(double distance, double fogDensity) {
    // cesium-native: exp(-(distance * fogDensity)^2) > 0.0
    if (fogDensity <= 0.0) return true;
    double fogScalar = distance * fogDensity;
    return std::exp(-(fogScalar * fogScalar)) > 0.0;
}

namespace {

constexpr double kTerrainMapQuality = 0.25;
constexpr double kTerrainMapWidth = 65.0;
constexpr double kDefaultTerrainMinimumHeight = -1000.0;
constexpr double kDefaultTerrainMaximumHeight = 9000.0;
constexpr double kPiForLongitudeWrap = 3.14159265358979323846264338327950288;
constexpr double kTwoPiForLongitudeWrap = kPiForLongitudeWrap * 2.0;
constexpr double kPostInteractionResourceSmoothingSeconds = 1.25;
constexpr int kSmoothedMainThreadUploadLimit = 1;
constexpr int kActiveInteractionRenderPrepBudget = 0;
constexpr int kRecoveryRenderPrepBudget = 1;

int rasterTextureSourceZoom(const RasterOverlayTile* tile) {
    if (!tile) return -1;
    return tile->isRectangleTile() ? tile->getSourceZoom() : tile->getTileID().z;
}

std::optional<std::array<float, 4>> clipUvForDescendantBounds(
    const Rectangle& ancestorBounds,
    const Rectangle& descendantBounds) {
    const double ancestorWidth = ancestorBounds.width();
    const double ancestorHeight = ancestorBounds.height();
    if (ancestorWidth <= 0.0 || ancestorHeight <= 0.0) {
        return std::nullopt;
    }

    auto longitudeOffset = [&](double longitude) {
        double offset = longitude - ancestorBounds.west();
        if (ancestorBounds.crossesAntimeridian() && offset < 0.0) {
            offset += kTwoPiForLongitudeWrap;
        }
        return offset;
    };

    double uMin = longitudeOffset(descendantBounds.west()) / ancestorWidth;
    double uMax = longitudeOffset(descendantBounds.east()) / ancestorWidth;
    if (ancestorBounds.crossesAntimeridian() && uMax < uMin) {
        uMax += 1.0;
    }
    double vMin = (ancestorBounds.north() - descendantBounds.north()) /
                  ancestorHeight;
    double vMax = (ancestorBounds.north() - descendantBounds.south()) /
                  ancestorHeight;

    uMin = std::clamp(uMin, 0.0, 1.0);
    uMax = std::clamp(uMax, 0.0, 1.0);
    vMin = std::clamp(vMin, 0.0, 1.0);
    vMax = std::clamp(vMax, 0.0, 1.0);
    if (uMax <= uMin || vMax <= vMin) {
        return std::nullopt;
    }

    return std::array<float, 4>{
        static_cast<float>(uMin),
        static_cast<float>(vMin),
        static_cast<float>(uMax - uMin),
        static_cast<float>(vMax - vMin)};
}

bool overlayBindingAllowedByPolicy(
    const ActivatedRasterOverlay* activeOverlay,
    const RasterMappedToTilesetTile* mapped,
    const SurfaceRasterBinding& binding) {
    if (!activeOverlay || !activeOverlay->visible() ||
        !mapped || binding.kind == SurfaceRasterBindingKind::None ||
        !binding.tile || !binding.tile->getTexture()) {
        return false;
    }
    if (activeOverlay->getOverlay().role() ==
            RasterOverlayRole::BaseImagery) {
        return true;
    }
    if (activeOverlay->getOverlay().fallbackPolicy() ==
        RasterOverlayFallbackPolicy::SkipUntilReady) {
        return mapped->getLoadingTile() == nullptr;
    }
    return true;
}

double terrainHeightPadding(double minimumHeight, double maximumHeight) {
    return std::max(std::abs(minimumHeight), std::abs(maximumHeight));
}

double terrainHeightPadding(const TilesetTile& tile) {
    if (!tile.hasTerrainHeightRange) {
        return terrainHeightPadding(
            kDefaultTerrainMinimumHeight,
            kDefaultTerrainMaximumHeight);
    }
    return terrainHeightPadding(
        tile.terrainMinimumHeight,
        tile.terrainMaximumHeight);
}

double computeApproximateDistanceToTileBounds(const Rectangle& bounds,
                                              const Vec3& cameraPosition,
                                              double heightPadding);
std::optional<OrientedBoundingBox> computeBoundingRegionObb(
    const Rectangle& bounds,
    double minimumHeight,
    double maximumHeight);

double terrainMinimumHeight(const TilesetTile& tile) {
    return tile.hasTerrainHeightRange
        ? tile.terrainMinimumHeight
        : kDefaultTerrainMinimumHeight;
}

double terrainMaximumHeight(const TilesetTile& tile) {
    return tile.hasTerrainHeightRange
        ? tile.terrainMaximumHeight
        : kDefaultTerrainMaximumHeight;
}

FrameResourceBudgetConfig makeFrameResourceBudgetConfig(
    const TilesetOptions& options,
    bool interactionActive,
    bool resourceSmoothingActive) {
    FrameResourceBudgetConfig config;
    config.maxNetworkRequestsPerFrame = options.maximumSimultaneousTileLoads;
    config.maxTerrainContentNetworkRequestsPerFrame =
        options.maximumSimultaneousTileLoads;
    config.maxRasterNetworkRequestsPerFrame =
        std::max<uint32_t>(
            64u,
            static_cast<uint32_t>(options.maximumSimultaneousTileLoads) * 4u);
    config.maxNetworkInflight = options.maximumSimultaneousTileLoads;
    config.maxTerrainContentNetworkInflight =
        options.maximumSimultaneousTileLoads;
    config.maxRasterNetworkInflight = config.maxRasterNetworkRequestsPerFrame;
    config.maxMainThreadFinalizesPerFrame =
        resourceSmoothingActive
            ? static_cast<uint32_t>(kSmoothedMainThreadUploadLimit)
            : options.maximumSimultaneousTileLoads;
    config.maxRasterUploadsPerFrame =
        resourceSmoothingActive
            ? std::min<uint32_t>(4u, options.maximumSimultaneousTileLoads)
            : options.maximumSimultaneousTileLoads;
    config.mainThreadTimeMs = options.mainThreadLoadingTimeLimit;
    config.interactionActive = interactionActive;
    config.smoothingActive = resourceSmoothingActive;
    return config;
}

void setTerrainHeightRange(TilesetTile& tile,
                           double minimumHeight,
                           double maximumHeight) {
    tile.hasTerrainHeightRange = true;
    tile.terrainMinimumHeight = minimumHeight;
    tile.terrainMaximumHeight = maximumHeight;
}

void setDefaultTerrainHeightRange(TilesetTile& tile) {
    setTerrainHeightRange(
        tile,
        kDefaultTerrainMinimumHeight,
        kDefaultTerrainMaximumHeight);
}

void inheritTerrainHeightRange(TilesetTile& child,
                               const TilesetTile& parent) {
    if (parent.hasTerrainHeightRange) {
        setTerrainHeightRange(
            child,
            parent.terrainMinimumHeight,
            parent.terrainMaximumHeight);
    } else {
        setDefaultTerrainHeightRange(child);
    }
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

Vec3 tileBoundsCenterFromRectangle(const Rectangle& bounds) {
    const auto& ellipsoid = Ellipsoid::WGS84();
    const double centerLng = (bounds.west() + bounds.east()) * 0.5;
    const double centerLat = (bounds.south() + bounds.north()) * 0.5;
    return ellipsoid.cartographicToCartesian(
        Cartographic::fromRadians(centerLng, centerLat, 0.0));
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

double cesiumTerrainGeometricError(const Rectangle& bounds) {
    // cesium-native LayerJsonTerrainLoader:
    // 8.0 * calcQuadtreeMaxGeometricError(ellipsoid) * rectangle.computeWidth()
    const double maxGeometricErrorPerRadian =
        Ellipsoid::WGS84().semiMajorAxis() * kTerrainMapQuality / kTerrainMapWidth;
    return 8.0 * maxGeometricErrorPerRadian * bounds.width();
}

struct RasterTargetScreenPixels {
    double x = 256.0;
    double y = 256.0;
};

bool signsDifferOrTouchesZero(double a, double b) {
    return a == 0.0 || b == 0.0 || (a < 0.0) != (b < 0.0);
}

double distanceOnEllipsoid(double lngA, double latA,
                           double lngB, double latB) {
    const auto& ellipsoid = Ellipsoid::WGS84();
    const Vec3 a = ellipsoid.cartographicToCartesian(
        Cartographic::fromRadians(lngA, latA, 0.0));
    const Vec3 b = ellipsoid.cartographicToCartesian(
        Cartographic::fromRadians(lngB, latB, 0.0));
    return a.distanceTo(b);
}

RasterTargetScreenPixels computeProjectedRectangleSizeMeters(
    const Rectangle& bounds) {
    const double west = bounds.west();
    const double east = bounds.east();
    const double south = bounds.south();
    const double north = bounds.north();
    const double centerLng = west + bounds.width() * 0.5;

    const double lowerDistance = distanceOnEllipsoid(west, south, east, south);
    const double upperDistance = distanceOnEllipsoid(west, north, east, north);
    const double leftDistance = distanceOnEllipsoid(west, south, west, north);
    const double rightDistance = distanceOnEllipsoid(east, south, east, north);

    RasterTargetScreenPixels size;
    size.x = std::max(lowerDistance, upperDistance);
    size.y = std::max(leftDistance, rightDistance);

    // cesium-native Projection::computeProjectedRectangleSize: rectangles
    // spanning a large longitudinal range can have endpoints close together in
    // Cartesian space, so also check distances through the midpoint.
    const double halfDistanceLA =
        distanceOnEllipsoid(west, south, centerLng, south);
    const double halfDistanceLB =
        distanceOnEllipsoid(centerLng, south, east, south);
    const double halfDistanceUA =
        distanceOnEllipsoid(west, north, centerLng, north);
    const double halfDistanceUB =
        distanceOnEllipsoid(centerLng, north, east, north);
    if (halfDistanceLA > size.x || halfDistanceLB > size.x ||
        halfDistanceUA > size.x || halfDistanceUB > size.x) {
        size.x = std::max(halfDistanceLA + halfDistanceLB,
                          halfDistanceUA + halfDistanceUB);
    }

    if (signsDifferOrTouchesZero(west, east)) {
        size.y = std::max(
            size.y,
            distanceOnEllipsoid(0.0, south, 0.0, north));
    }

    if (signsDifferOrTouchesZero(south, north)) {
        const double equatorDistance =
            distanceOnEllipsoid(west, 0.0, east, 0.0);
        size.x = std::max(size.x, equatorDistance);

        const double halfDistanceL =
            distanceOnEllipsoid(west, 0.0, centerLng, 0.0);
        const double halfDistanceR =
            distanceOnEllipsoid(centerLng, 0.0, east, 0.0);
        if (halfDistanceL > size.x || halfDistanceR > size.x) {
            size.x = halfDistanceL + halfDistanceR;
        }
    }

    return size;
}

RasterTargetScreenPixels computeDesiredRasterScreenPixels(
    const Rectangle& bounds,
    double geometricError,
    double maximumScreenSpaceError) {
    if (geometricError <= 0.0) return {};

    RasterTargetScreenPixels diameters =
        computeProjectedRectangleSizeMeters(bounds);
    diameters.x = std::max(
        1.0,
        diameters.x * maximumScreenSpaceError /
            geometricError);
    diameters.y = std::max(
        1.0,
        diameters.y * maximumScreenSpaceError /
            geometricError);
    return diameters;
}

int geographicTmsXCount(int z) {
    return 1 << (z + 1);
}

int geographicTmsYCount(int z) {
    return 1 << z;
}

std::vector<FrameState::SelectorView> buildSelectorViews(
    const FrameState& frameState) {
    return frameState.selectorViews;
}

float sampleHeightFromDecodedTile(const DecodedHeightmap& heightmap,
                                  const Rectangle& sourceBounds,
                                  double lngRad,
                                  double latRad) {
    if (!heightmap.valid()) return 0.0f;

    double u = (lngRad - sourceBounds.west()) / sourceBounds.width();
    double v = (sourceBounds.north() - latRad) / sourceBounds.height();
    constexpr double kTileCoordinateEpsilon = 1e-12;
    const auto clampTileCoordinate = [](double& coordinate) {
        if (coordinate < -kTileCoordinateEpsilon ||
            coordinate > 1.0 + kTileCoordinateEpsilon) {
            return false;
        }
        coordinate = std::clamp(coordinate, 0.0, 1.0);
        return true;
    };
    if (!clampTileCoordinate(u) || !clampTileCoordinate(v)) {
        return 0.0f;
    }

    const float h = heightmap.sampleBilinear(
        static_cast<float>(u), static_cast<float>(v));
    if (heightmap.isNoData(h)) return 0.0f;
    return h;
}

void kickVisitedDescendants(TilesetTile& tile) {
    for (TilesetTile* child : tile.children) {
        if (!child) continue;
        kickSelectionState(child->selectionState);
        kickVisitedDescendants(*child);
    }
}

TextureDesc::Filter toTextureFilter(GltfTextureFilter filter) {
    return filter == GltfTextureFilter::Nearest
        ? TextureDesc::Filter::Nearest
        : TextureDesc::Filter::Linear;
}

TextureDesc::Wrap toTextureWrap(GltfTextureWrap wrap) {
    switch (wrap) {
        case GltfTextureWrap::ClampToEdge:
            return TextureDesc::Wrap::Clamp;
        case GltfTextureWrap::MirroredRepeat:
            return TextureDesc::Wrap::MirroredRepeat;
        case GltfTextureWrap::Repeat:
        default:
            return TextureDesc::Wrap::Repeat;
    }
}

float alphaModeUniform(GltfAlphaMode mode) {
    switch (mode) {
        case GltfAlphaMode::Mask:
            return 1.0f;
        case GltfAlphaMode::Blend:
            return 2.0f;
        case GltfAlphaMode::Opaque:
        default:
            return 0.0f;
    }
}

RenderCommand::PrimitiveType renderPrimitiveType(GltfPrimitiveMode mode) {
    switch (mode) {
        case GltfPrimitiveMode::Points:
            return RenderCommand::PrimitiveType::Points;
        case GltfPrimitiveMode::Lines:
            return RenderCommand::PrimitiveType::Lines;
        case GltfPrimitiveMode::LineStrip:
            return RenderCommand::PrimitiveType::LineStrip;
        case GltfPrimitiveMode::Triangles:
            return RenderCommand::PrimitiveType::Triangles;
    }
    return RenderCommand::PrimitiveType::Triangles;
}

std::unique_ptr<Texture> createGltfGpuTexture(RenderDevice* device,
                                              const GltfTexture& texture) {
    if (!device ||
        texture.image.width <= 0 ||
        texture.image.height <= 0 ||
        texture.image.pixels.empty()) {
        return nullptr;
    }

    const size_t pixelCount =
        static_cast<size_t>(texture.image.width) *
        static_cast<size_t>(texture.image.height);
    std::vector<uint8_t> rgbaPixels;
    const uint8_t* texturePixels = texture.image.pixels.data();
    size_t texturePixelBytes = texture.image.pixels.size();
    if (texture.image.channels == 3) {
        if (texture.image.pixels.size() < pixelCount * 3u) {
            return nullptr;
        }
        rgbaPixels.resize(pixelCount * 4u);
        for (size_t i = 0; i < pixelCount; ++i) {
            rgbaPixels[i * 4u + 0u] = texture.image.pixels[i * 3u + 0u];
            rgbaPixels[i * 4u + 1u] = texture.image.pixels[i * 3u + 1u];
            rgbaPixels[i * 4u + 2u] = texture.image.pixels[i * 3u + 2u];
            rgbaPixels[i * 4u + 3u] = 255u;
        }
        texturePixels = rgbaPixels.data();
        texturePixelBytes = rgbaPixels.size();
    } else if (texture.image.channels == 4) {
        if (texture.image.pixels.size() < pixelCount * 4u) {
            return nullptr;
        }
    } else {
        return nullptr;
    }

    TextureDesc textureDesc;
    textureDesc.width = texture.image.width;
    textureDesc.height = texture.image.height;
    textureDesc.format = TextureDesc::Format::RGBA8;
    textureDesc.data = texturePixels;
    textureDesc.dataSize = texturePixelBytes;
    textureDesc.mipmap = texture.sampler.mipmap;
    textureDesc.minFilter = toTextureFilter(texture.sampler.minFilter);
    textureDesc.magFilter = toTextureFilter(texture.sampler.magFilter);
    textureDesc.wrapS = toTextureWrap(texture.sampler.wrapS);
    textureDesc.wrapT = toTextureWrap(texture.sampler.wrapT);
    return device->createTexture(textureDesc);
}

GltfPrimitiveRenderResources::TextureBinding makeGltfTextureBinding(
    const std::optional<GltfTextureBinding>& modelBinding,
    const std::vector<std::unique_ptr<Texture>>& tileTextures) {
    GltfPrimitiveRenderResources::TextureBinding renderBinding;
    if (!modelBinding ||
        modelBinding->textureIndex >= tileTextures.size() ||
        !tileTextures[modelBinding->textureIndex]) {
        return renderBinding;
    }
    renderBinding.texture = tileTextures[modelBinding->textureIndex].get();
    renderBinding.texCoord = modelBinding->texCoord;
    renderBinding.offsetScale = {
        modelBinding->transform.offset[0],
        modelBinding->transform.offset[1],
        modelBinding->transform.scale[0],
        modelBinding->transform.scale[1]};
    renderBinding.rotationSinCos = {
        static_cast<float>(std::sin(modelBinding->transform.rotation)),
        static_cast<float>(std::cos(modelBinding->transform.rotation))};
    return renderBinding;
}

} // namespace

Vec3 Tileset::tileBoundsCenter(const Rectangle& bounds) {
    return tileBoundsCenterFromRectangle(bounds);
}

namespace {

Vec3 explicitBoundingVolumeCenter(const TileBoundingVolume& volume) {
    switch (volume.kind) {
        case TileBoundingVolumeKind::Region:
            return tileBoundsCenterFromRectangle(volume.region);
        case TileBoundingVolumeKind::Sphere:
            return volume.sphere.getCenter();
        case TileBoundingVolumeKind::Box:
            return volume.box.getCenter();
    }
    return Vec3::zero();
}

double explicitBoundingVolumeDistance(const TileBoundingVolume& volume,
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
    }
    return 0.0;
}

bool explicitBoundingVolumeContainsPosition(const TileBoundingVolume& volume,
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
    }
    return false;
}

bool viewerRequestVolumeAllowsTile(
    const TilesetTile& tile,
    const SelectorFrame& selectorFrame) {
    if (!tile.viewerRequestVolume) {
        return true;
    }
    for (const auto& view : selectorFrame.views) {
        if (explicitBoundingVolumeContainsPosition(
                *tile.viewerRequestVolume,
                view.position)) {
            return true;
        }
    }
    return false;
}

bool matricesNearlyEqual(const Mat4& lhs,
                         const Mat4& rhs,
                         double epsilon) {
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            if (std::abs(lhs(row, col) - rhs(row, col)) > epsilon) {
                return false;
            }
        }
    }
    return true;
}

bool explicitBoundingVolumeIntersectsFrustum(
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
    }
    return false;
}

} // namespace

double Tileset::tileBoundsRadius(const TilesetTile& tile,
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
        }
    }
    return computeTileBoundsRadius(
        tile.bounds,
        center,
        terrainHeightPadding(tile));
}

std::optional<OrientedBoundingBox> Tileset::tileBoundingRegionObb(
    const TilesetTile& tile) {
    if (tile.boundingVolume) {
        if (tile.boundingVolume->kind == TileBoundingVolumeKind::Region) {
            return computeBoundingRegionObb(
                tile.boundingVolume->region,
                tile.boundingVolume->minimumHeight,
                tile.boundingVolume->maximumHeight);
        }
        if (tile.boundingVolume->kind == TileBoundingVolumeKind::Box) {
            return tile.boundingVolume->box;
        }
    }
    return computeBoundingRegionObb(
        tile.bounds,
        terrainMinimumHeight(tile),
        terrainMaximumHeight(tile));
}

bool Tileset::tileIntersectsFrustum(const TilesetTile& tile,
                                    const Frustum& frustum) {
    if (tile.boundingVolume) {
        return explicitBoundingVolumeIntersectsFrustum(
            *tile.boundingVolume,
            frustum);
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

double Tileset::approximateDistanceToTileBounds(
    const TilesetTile& tile,
    const Vec3& cameraPosition) {
    if (tile.boundingVolume) {
        return explicitBoundingVolumeDistance(
            *tile.boundingVolume,
            cameraPosition);
    }
    const double distanceSquared = computeBoundingRegionDistanceSquared(
        tile.bounds,
        terrainMinimumHeight(tile),
        terrainMaximumHeight(tile),
        cameraPosition);
    return std::sqrt(std::max(0.0, distanceSquared));
}

// ── cesium-native priority implementation ──

double Tileset::computeTilePriority(const Vec3& tileCenter,
                                     const Vec3& cameraPos,
                                     const Vec3& cameraDir,
                                     double distance) {
    // cesium-native: (1.0 - dot(tileDirection, viewDirection)) * distance
    // Lower = higher load priority.
    Vec3 tileDirection = tileCenter - cameraPos;
    double magnitude = tileDirection.length();
    if (magnitude < 1e-5) return distance;  // degenerate: at camera position

    tileDirection = tileDirection / magnitude;
    double viewDot = tileDirection.dot(cameraDir);
    // Clamp to [-1, 1] to handle floating-point error
    viewDot = std::max(-1.0, std::min(1.0, viewDot));
    return (1.0 - viewDot) * distance;
}

// ── cesium-native cache implementation ──

namespace {

int64_t estimateHeightmapBytes(const DecodedHeightmap& heightmap) {
    int64_t bytes = 0;
    bytes += static_cast<int64_t>(heightmap.rawData.size());
    bytes += static_cast<int64_t>(
        heightmap.heights.size() * sizeof(float));
    bytes += static_cast<int64_t>(
        heightmap.noDataValues.size() * sizeof(float));
    bytes += static_cast<int64_t>(
        heightmap.metadataAvailability.size() * sizeof(std::array<int, 5>));
    for (const auto& update : heightmap.quantizedMeshAvailabilityUpdates) {
        bytes += static_cast<int64_t>(
            sizeof(DecodedHeightmap::QuantizedMeshAvailabilityUpdate));
        bytes += static_cast<int64_t>(update.subtreeKey.schemeId.size());
        bytes += static_cast<int64_t>(
            update.metadataAvailability.size() * sizeof(std::array<int, 5>));
    }
    if (heightmap.surfaceMesh) {
        bytes += static_cast<int64_t>(
            heightmap.surfaceMesh->vertices.size() * sizeof(SurfaceVertex));
        bytes += static_cast<int64_t>(
            heightmap.surfaceMesh->indices.size() * sizeof(uint32_t));
        bytes += static_cast<int64_t>(
            heightmap.surfaceMesh->gpuVertices.size() *
            sizeof(SurfaceGpuVertex));
    }
    return bytes;
}

} // namespace

int64_t Tileset::estimateTileBytes(const TilesetTile& tile) {
    int64_t bytes = 0;
    if (tile.mesh) {
        bytes += static_cast<int64_t>(
            tile.mesh->vertices.size() * sizeof(SurfaceVertex));
        bytes += static_cast<int64_t>(
            tile.mesh->indices.size() * sizeof(uint32_t));
        bytes += static_cast<int64_t>(tile.mesh->waterMask.data.size());
        bytes += static_cast<int64_t>(
            tile.mesh->metadataAvailability.size() *
            sizeof(std::array<int, 5>));
    }
    if (tile.gltfModel) {
        bytes += tile.gltfModel->byteSize();
    }
    if (tile.gpuVertexBuffer) {
        bytes += static_cast<int64_t>(tile.gpuVertexBuffer->size());
    }
    if (tile.gpuIndexBuffer) {
        bytes += static_cast<int64_t>(tile.gpuIndexBuffer->size());
    }
    for (const std::unique_ptr<Texture>& texture : tile.gltfTextureResources) {
        if (texture) {
            bytes += static_cast<int64_t>(
                texture->width() * texture->height() * 4);
        }
    }
    for (const GltfPrimitiveRenderResources& primitive :
         tile.gltfPrimitiveResources) {
        if (primitive.vertexBuffer) {
            bytes += static_cast<int64_t>(primitive.vertexBuffer->size());
        }
        if (primitive.indexBuffer) {
            bytes += static_cast<int64_t>(primitive.indexBuffer->size());
        }
    }
    if (tile.heightmap) {
        bytes += estimateHeightmapBytes(*tile.heightmap);
    }
    // Raster overlays: count textures through the retained ready tile, so the
    // Texture* remains owned while the estimate reads its dimensions.
    for (const auto& overlay : tile.rasterOverlays) {
        const std::shared_ptr<RasterOverlayTile> readyTile =
            overlay ? overlay->getReadyTileHandle() : nullptr;
        Texture* texture = readyTile ? readyTile->getTexture() : nullptr;
        if (!texture) {
            continue;
        }
#ifdef __ANDROID__
        // Android lifetime guard: this estimator runs after raster provider trimming.
        // Avoid crashing on retained raw pointers while logging the stale state.
        if (!RasterOverlayTileProvider::isLiveTextureForLifetimeGuard(texture)) {
            static int staleTextureLogCount = 0;
            if (staleTextureLogCount < 20) {
                __android_log_print(
                    ANDROID_LOG_WARN,
                    "EarthPerfCrash",
                    "stale raster texture in estimateTileBytes tile=%s/%d/%d/%d state=%d readyTile=%p texture=%p",
                    tile.key.schemeId.c_str(),
                    tile.key.z,
                    tile.key.x,
                    tile.key.y,
                    static_cast<int>(overlay->getState()),
                    static_cast<const void*>(readyTile.get()),
                    static_cast<void*>(texture));
                ++staleTextureLogCount;
            }
            continue;
        }
#endif
        bytes += static_cast<int64_t>(texture->width() *
                                      texture->height() * 4);
    }
    return bytes;
}

void Tileset::updateTotalBytesUsed() {
    // cesium-native: recompute total bytes from current tile state.
    // More robust than incremental tracking because overlay textures
    // attach/detach asynchronously, and terrain cache grows independently.
    totalBytesUsed_ = 0;

    // Tile geometry + raster overlay textures
    for (const auto& [key, tile] : tiles_) {
        totalBytesUsed_ += estimateTileBytes(*tile);
    }

    // Terrain cache (DecodedHeightmap raw data)
    for (const auto& [key, hm] : terrainCache_) {
        if (hm) {
            totalBytesUsed_ += estimateHeightmapBytes(*hm);
        }
    }
}

namespace {

bool hasUnloadableTileContent(const TilesetTile& tile) {
    if (tile.contentKind == TileContentKind::Unknown) {
        return false;
    }
    if (tile.loadState == TileLoadState::Unloaded ||
        tile.loadState == TileLoadState::ContentLoading ||
        tile.loadState == TileLoadState::Unloading) {
        return false;
    }
    return true;
}

bool hasContentLoadingUpsampledDescendant(const TilesetTile& tile) {
    for (const TilesetTile* child : tile.children) {
        if (!child) continue;
        if (child->upsampledFromParent &&
            child->loadState == TileLoadState::ContentLoading) {
            return true;
        }
        if (hasContentLoadingUpsampledDescendant(*child)) {
            return true;
        }
    }
    return false;
}

void unloadMainThreadRenderResources(TilesetTile& tile) {
    tile.gpuVertexBuffer.reset();
    tile.gpuIndexBuffer.reset();
    tile.gltfTextureResources.clear();
    tile.gltfPrimitiveResources.clear();
    tile.surfaceDrawable = false;
    tile.surfaceSource = SurfaceDrawableSource::None;
    tile.completeRenderable = false;
    tile.renderable = false;
}

} // namespace

void Tileset::markEligibleForUnloading(const std::string& key) {
    // cesium-native: add to back of LRU queue if not already present
    auto tileIt = tiles_.find(key);
    if (tileIt == tiles_.end() ||
        !tileIt->second ||
        !hasUnloadableTileContent(*tileIt->second)) {
        return;
    }
    if (unloadQueueMap_.count(key)) return;
    unloadQueue_.push_back(key);
    unloadQueueMap_[key] = --unloadQueue_.end();
}

void Tileset::markIneligibleForUnloading(const std::string& key) {
    // cesium-native: remove from unload queue (tile was used this frame)
    auto it = unloadQueueMap_.find(key);
    if (it != unloadQueueMap_.end()) {
        unloadQueue_.erase(it->second);
        unloadQueueMap_.erase(it);
    }
}

bool Tileset::hasReferencedDescendant(const TilesetTile& tile) const {
    for (const TilesetTile* child : tile.children) {
        if (!child) continue;
        if (child->referenceCount() > 0 ||
            hasReferencedDescendant(*child)) {
            return true;
        }
    }
    return false;
}

bool Tileset::subtreeHasActiveContentWork(const TilesetTile& tile) {
    std::vector<std::string> keys;
    keys.push_back(terrainCacheKey(tile.key));
    std::vector<const TilesetTile*> stack;
    stack.reserve(tile.children.size());
    for (const TilesetTile* child : tile.children) {
        if (child) stack.push_back(child);
    }

    while (!stack.empty()) {
        const TilesetTile* current = stack.back();
        stack.pop_back();
        keys.push_back(terrainCacheKey(current->key));
        for (const TilesetTile* child : current->children) {
            if (child) stack.push_back(child);
        }
    }

    std::lock_guard<std::mutex> lock(pendingMutex_);
    for (const std::string& key : keys) {
        if (pendingRequests_.count(key) ||
            pendingUploadKeys_.count(key) ||
            pendingContentUploadKeys_.count(key)) {
            return true;
        }
        const auto uploadIt = std::find_if(
            pendingUploads_.begin(),
            pendingUploads_.end(),
            [&key](const PendingTerrainUpload& upload) {
                return upload.cacheKey == key;
            });
        if (uploadIt != pendingUploads_.end()) {
            return true;
        }
        const auto terminalIt = std::find_if(
            pendingTerminalResults_.begin(),
            pendingTerminalResults_.end(),
            [&key](const PendingTerrainTerminalResult& result) {
                return result.cacheKey == key;
            });
        if (terminalIt != pendingTerminalResults_.end()) {
            return true;
        }
        const auto contentUploadIt = std::find_if(
            pendingContentUploads_.begin(),
            pendingContentUploads_.end(),
            [&key](const PendingContentUpload& upload) {
                return upload.cacheKey == key;
            });
        if (contentUploadIt != pendingContentUploads_.end()) {
            return true;
        }
        const auto contentTerminalIt = std::find_if(
            pendingContentTerminalResults_.begin(),
            pendingContentTerminalResults_.end(),
            [&key](const PendingContentTerminalResult& result) {
                return result.cacheKey == key;
            });
        if (contentTerminalIt != pendingContentTerminalResults_.end()) {
            return true;
        }
    }
    return false;
}

void Tileset::eraseTileIndexState(const std::string& key) {
    markIneligibleForUnloading(key);
    terrainCache_.erase(key);
    emptyTiles_.erase(key);
    loadQueue_.erase(
        std::remove_if(
            loadQueue_.begin(),
            loadQueue_.end(),
            [this, &key](const TileLoadRequest& request) {
                return terrainCacheKey(request.key) == key;
            }),
        loadQueue_.end());

    std::lock_guard<std::mutex> lock(pendingMutex_);
    if (auto tokenIt = pendingRequestTokens_.find(key);
        tokenIt != pendingRequestTokens_.end()) {
        tokenIt->second.cancel();
    }
    pendingUploadKeys_.erase(key);
    pendingContentRequestKeys_.erase(key);
    pendingContentUploadKeys_.erase(key);
    pendingUploads_.erase(
        std::remove_if(
            pendingUploads_.begin(),
            pendingUploads_.end(),
            [&key](const PendingTerrainUpload& upload) {
                return upload.cacheKey == key;
            }),
        pendingUploads_.end());
    pendingTerminalResults_.erase(
        std::remove_if(
            pendingTerminalResults_.begin(),
            pendingTerminalResults_.end(),
            [&key](const PendingTerrainTerminalResult& result) {
                return result.cacheKey == key;
            }),
        pendingTerminalResults_.end());
    pendingContentUploads_.erase(
        std::remove_if(
            pendingContentUploads_.begin(),
            pendingContentUploads_.end(),
            [&key](const PendingContentUpload& upload) {
                return upload.cacheKey == key;
            }),
        pendingContentUploads_.end());
    pendingContentTerminalResults_.erase(
        std::remove_if(
            pendingContentTerminalResults_.begin(),
            pendingContentTerminalResults_.end(),
            [&key](const PendingContentTerminalResult& result) {
                return result.cacheKey == key;
            }),
        pendingContentTerminalResults_.end());
}

void Tileset::clearChildrenRecursively(TilesetTile* tile,
                                        IPrepareRendererResources* pPrepRenderer) {
    if (!tile) return;
    std::vector<TilesetTile*> children = tile->children;
    for (auto* child : children) {
        if (!child) continue;
        const std::string childKey = terrainCacheKey(child->key);
        // cesium-native: detach via real IPrepareRendererResources
        for (auto& overlay : child->rasterOverlays) {
            if (overlay) overlay->releaseTileReferences(pPrepRenderer);
        }
        clearChildrenRecursively(child, pPrepRenderer);
        child->parent = nullptr;
        eraseTileIndexState(childKey);
        tiles_.erase(childKey);
    }
    tile->children.clear();
}

void Tileset::detachInactiveRasterOverlays(
    IPrepareRendererResources* pPrepRenderer) {
    for (auto& [ck, tile] : tiles_) {
        if (!tile || tile->lastUsedFrame == frameNumber_) continue;
        for (auto& overlay : tile->rasterOverlays) {
            if (overlay) {
                overlay->releaseTileReferences(pPrepRenderer);
            }
        }
    }
}

Tileset::UnloadTileContentResult Tileset::unloadTileContent(
    TilesetTile& tile,
    IPrepareRendererResources* pPrepRenderer) {
    if (tile.loadState == TileLoadState::Unloaded) {
        return UnloadTileContentResult::Remove;
    }

    if (tile.loadState == TileLoadState::ContentLoading) {
        return UnloadTileContentResult::Keep;
    }

    for (auto& overlay : tile.rasterOverlays) {
        if (overlay) {
            overlay->releaseTileReferences(pPrepRenderer);
        }
    }
    tile.rasterOverlays.clear();

    const std::string key = terrainCacheKey(tile.key);
    UnloadTileContentResult result = UnloadTileContentResult::Remove;
    switch (tile.contentKind) {
        case TileContentKind::External:
            result = UnloadTileContentResult::RemoveAndClearChildren;
            break;
        case TileContentKind::Render:
            if (tile.loadState != TileLoadState::Unloading &&
                hasContentLoadingUpsampledDescendant(tile)) {
                unloadMainThreadRenderResources(tile);
                tile.loadState = TileLoadState::Unloading;
                return UnloadTileContentResult::Keep;
            }
            if (tile.loadState == TileLoadState::Unloading &&
                hasContentLoadingUpsampledDescendant(tile)) {
                return UnloadTileContentResult::Keep;
            }
            tile.mesh.reset();
            tile.gltfModel.reset();
            tile.gltfContentTransform = Mat4::identity();
            tile.gltfTextureResources.clear();
            tile.gltfPrimitiveResources.clear();
            tile.gpuVertexBuffer.reset();
            tile.gpuIndexBuffer.reset();
            tile.meshReady = false;
            tile.surfaceDrawable = false;
            tile.surfaceSource = SurfaceDrawableSource::None;
            terrainCache_.erase(key);
            break;
        case TileContentKind::Empty:
            emptyTiles_.erase(key);
            break;
        case TileContentKind::Unknown:
            break;
    }

    tile.contentKind = TileContentKind::Unknown;
    tile.loadState = TileLoadState::Unloaded;
    tile.completeRenderable = false;
    tile.renderable = false;
    return result;
}

void Tileset::unloadCachedBytes(int64_t maximumCachedBytes,
                               IPrepareRendererResources* pPrepRenderer) {
    // cesium-native: unload from head of LRU queue until under budget
    const double unloadStartMs = perf::nowMs();
    const double timeBudgetMs = options_.tileCacheUnloadTimeLimit;
    const auto timeBudgetExpired = [&]() {
        return timeBudgetMs > 0.0 &&
               perf::nowMs() - unloadStartMs >= timeBudgetMs;
    };
    const auto hasQueuedUnloadingTile = [&]() {
        for (const std::string& queuedKey : unloadQueue_) {
            auto tileIt = tiles_.find(queuedKey);
            if (tileIt != tiles_.end() &&
                tileIt->second &&
                tileIt->second->loadState == TileLoadState::Unloading) {
                return true;
            }
        }
        return false;
    };

    size_t remainingCandidates = unloadQueue_.size();
    while ((totalBytesUsed_ > maximumCachedBytes ||
            hasQueuedUnloadingTile()) &&
           !unloadQueue_.empty() &&
           remainingCandidates > 0) {
        --remainingCandidates;
        const std::string key = unloadQueue_.front();
        auto tileIt = tiles_.find(key);
        if (tileIt == tiles_.end()) {
            // Stale entry — remove from queue
            unloadQueueMap_.erase(key);
            unloadQueue_.pop_front();
            if (timeBudgetExpired()) break;
            continue;
        }

        TilesetTile& tile = *tileIt->second;
        const int64_t estimatedBytesBeforeUnload = estimateTileBytes(tile);

        // cesium-native: skip tiles still referenced by the renderer or by
        // referenced descendants. Native child references propagate to parents;
        // this flat-map tile store has to check the subtree explicitly.
        const bool activeWorkPreventsUnload =
            tile.contentKind == TileContentKind::External &&
            subtreeHasActiveContentWork(tile);
        if (tile.referenceCount() > 0 ||
            hasReferencedDescendant(tile) ||
            activeWorkPreventsUnload) {
            // Move to back of queue — will retry next frame after references drop
            unloadQueue_.pop_front();
            unloadQueue_.push_back(key);
            unloadQueueMap_[key] = --unloadQueue_.end();
            if (timeBudgetExpired()) break;
            continue;
        }

        const UnloadTileContentResult removed =
            unloadTileContent(tile, pPrepRenderer);
        if (removed == UnloadTileContentResult::Keep) {
            unloadQueue_.pop_front();
            unloadQueue_.push_back(key);
            unloadQueueMap_[key] = --unloadQueue_.end();
            if (timeBudgetExpired()) break;
            continue;
        }

        markIneligibleForUnloading(key);
        if (removed == UnloadTileContentResult::RemoveAndClearChildren) {
            clearChildrenRecursively(&tile, pPrepRenderer);
        }

        totalBytesUsed_ = std::max<int64_t>(
            0,
            totalBytesUsed_ - std::max<int64_t>(0, estimatedBytesBeforeUnload));
        cacheBytesDirty_ = true;

        if (timeBudgetExpired()) break;
    }

    if (cacheBytesDirty_ && !resourceSmoothingActiveForFrame_) {
        updateTotalBytesUsed();
        cacheBytesDirty_ = false;
    }
}

// ──────────────────────────────────────────────────────────────

Tileset::Tileset(std::unique_ptr<TerrainProvider> terrainProvider,
                 std::unique_ptr<TileScheme> tileScheme,
                 std::vector<ActivatedRasterOverlay*> rasterOverlays,
                 RenderDevice* device,
                 TilesetOptions options,
                 std::unique_ptr<TilesetContentProvider> contentProvider)
    : terrainProvider_(std::move(terrainProvider)),
      contentProvider_(std::move(contentProvider)),
      tileScheme_(std::move(tileScheme)),
      rasterOverlays_(std::move(rasterOverlays)),
      device_(device),
      options_(std::move(options)) {
    frameResourceBudget_.beginFrame(
        0,
        makeFrameResourceBudgetConfig(options_, false, false));
    for (ActivatedRasterOverlay* overlay : rasterOverlays_) {
        if (overlay) {
            overlay->ensureTileProvider(device_);
        }
    }
}

Tileset::~Tileset() {
    std::unique_lock<std::mutex> lock(pendingMutex_);
    destroying_ = true;
    for (auto& [ck, token] : pendingRequestTokens_) {
        token.cancel();
        (void)ck;
    }
    pendingUploads_.clear();
    pendingTerminalResults_.clear();
    pendingUploadKeys_.clear();
    pendingContentUploads_.clear();
    pendingContentTerminalResults_.clear();
    pendingContentUploadKeys_.clear();
    pendingContentRequestKeys_.clear();

    // cesium-native keeps TilesetContentManager alive while worker callbacks
    // complete. This local engine has synchronous destruction, so wait until
    // every callback has observed the destroyed state and left the callback.
    pendingCondition_.wait(lock, [this]() {
        return pendingRequests_.empty();
    });
    pendingRequestTokens_.clear();
}

int Tileset::pendingRequests() const {
    std::lock_guard<std::mutex> lock(pendingMutex_);
    return static_cast<int>(pendingRequests_.size());
}

void Tileset::setOcclusionCallback(OcclusionCallback callback) {
    occlusionCallback_ = std::move(callback);
}

void Tileset::clearOcclusionCallback() {
    occlusionCallback_ = nullptr;
}

TilesetLoadDiagnostics Tileset::loadDiagnostics() const {
    TilesetLoadDiagnostics diag;

    for (const TileLoadRequest& request : loadQueue_) {
        switch (request.group) {
            case TileLoadPriorityGroup::Preload:
                ++diag.loadQueuePreloadRequests;
                break;
            case TileLoadPriorityGroup::Normal:
                ++diag.loadQueueNormalRequests;
                break;
            case TileLoadPriorityGroup::Urgent:
                ++diag.loadQueueUrgentRequests;
                break;
        }
    }

    {
        std::lock_guard<std::mutex> lock(pendingMutex_);
        const size_t pendingContentRequests =
            pendingContentRequestKeys_.size();
        const size_t pendingTerrainRequests =
            pendingRequests_.size() > pendingContentRequests
                ? pendingRequests_.size() - pendingContentRequests
                : 0;
        diag.pendingTerrainRequests =
            static_cast<int>(pendingTerrainRequests);
        diag.pendingTerrainUploads =
            static_cast<int>(pendingUploads_.size());
        diag.pendingTerrainTerminalResults =
            static_cast<int>(pendingTerminalResults_.size());
        diag.pendingContentRequests =
            static_cast<int>(pendingContentRequests);
        diag.pendingContentUploads =
            static_cast<int>(pendingContentUploads_.size());
        diag.pendingContentTerminalResults =
            static_cast<int>(pendingContentTerminalResults_.size());
    }

    diag.unloadQueueTiles = static_cast<int>(unloadQueue_.size());

    for (const auto& [ck, tile] : tiles_) {
        if (!tile) continue;

        switch (tile->loadState) {
            case TileLoadState::Unloading:
                ++diag.loadUnloadingTiles;
                break;
            case TileLoadState::FailedTemporarily:
                ++diag.loadFailedTemporarilyTiles;
                break;
            case TileLoadState::Unloaded:
                ++diag.loadUnloadedTiles;
                break;
            case TileLoadState::ContentLoading:
                ++diag.loadContentLoadingTiles;
                break;
            case TileLoadState::ContentLoaded:
                ++diag.loadContentLoadedTiles;
                break;
            case TileLoadState::Done:
                ++diag.loadDoneTiles;
                break;
            case TileLoadState::Failed:
                ++diag.loadFailedTiles;
                break;
        }

        switch (tile->contentKind) {
            case TileContentKind::Unknown:
                ++diag.contentUnknownTiles;
                break;
            case TileContentKind::Empty:
                ++diag.contentEmptyTiles;
                break;
            case TileContentKind::External:
                ++diag.contentExternalTiles;
                break;
            case TileContentKind::Render:
                ++diag.contentRenderTiles;
                break;
        }
        diag.missingRasterOverlayProjections +=
            static_cast<int>(tile->missingRasterOverlayProjections.size());
        (void)ck;
    }

    return diag;
}

void Tileset::markTileResourcesDirty() {
    ++resourceRevision_;
    cacheBytesDirty_ = true;
    hasReusableSelection_ = false;
}

bool Tileset::hasTilesetPendingWork() const {
    std::lock_guard<std::mutex> lock(pendingMutex_);
    return !pendingRequests_.empty() ||
           !pendingUploads_.empty() ||
           !pendingTerminalResults_.empty() ||
           !pendingContentUploads_.empty() ||
           !pendingContentTerminalResults_.empty();
}

bool Tileset::hasRasterOverlayPendingWork() const {
    for (const auto* overlay : rasterOverlays_) {
        if (overlay && overlay->hasPendingWork()) {
            return true;
        }
    }
    return false;
}

uint64_t Tileset::rasterOverlayRevision() const {
    uint64_t revision = 1469598103934665603ull;
    for (const auto* overlay : rasterOverlays_) {
        revision ^= overlay ? overlay->revision() : 0;
        revision *= 1099511628211ull;
    }
    return revision;
}

uint64_t Tileset::overlayConfigurationSignature() const {
    uint64_t signature = 1469598103934665603ull;
    auto mix = [&signature](uint64_t value) {
        signature ^= value + 0x9e3779b97f4a7c15ull + (signature << 6) +
                     (signature >> 2);
    };

    mix(static_cast<uint64_t>(rasterOverlays_.size()));
    for (const auto* activeOverlay : rasterOverlays_) {
        if (!activeOverlay) {
            mix(0);
            continue;
        }

        const RasterOverlay& overlay = activeOverlay->getOverlay();
        const RasterOverlay::Options& options = overlay.getOptions();
        mix(activeOverlay->visible() ? 1ull : 0ull);
        mix(static_cast<uint64_t>(options.blocksCompleteRenderable ? 1 : 0));
        mix(static_cast<uint64_t>(options.role));
        mix(static_cast<uint64_t>(options.priority));
        mix(static_cast<uint64_t>(options.fallbackPolicy));
        mix(static_cast<uint64_t>(std::lround(
            static_cast<double>(activeOverlay->opacity()) * 1000000.0)));
        if (const RasterOverlayTileProvider* provider =
                activeOverlay->getTileProvider()) {
            mix(provider->isReady() ? 1ull : 0ull);
        } else {
            mix(0);
        }
    }
    return signature;
}

uint64_t Tileset::selectionResourceRevision() const {
    uint64_t revision = resourceRevision_;
    revision ^= rasterOverlayRevision() + 0x9e3779b97f4a7c15ull +
                (revision << 6) + (revision >> 2);
    return revision;
}

bool Tileset::selectorViewsEquivalent(
    const std::vector<FrameState::SelectorView>& lhs,
    const std::vector<FrameState::SelectorView>& rhs) const {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (size_t i = 0; i < lhs.size(); ++i) {
        const auto& a = lhs[i];
        const auto& b = rhs[i];
        if (a.position.distanceTo(b.position) > 1e-3) {
            return false;
        }
        if ((a.direction - b.direction).lengthSquared() > 1e-12) {
            return false;
        }
        if (!matricesNearlyEqual(
                a.projectionMatrix,
                b.projectionMatrix,
                1e-12)) {
            return false;
        }
        if (a.viewportHeightPixels != b.viewportHeightPixels) {
            return false;
        }
    }
    return true;
}

bool Tileset::canReuseSelection(
    const FrameState& frameState,
    uint64_t resourceRevision,
    uint64_t overlaySignature) const {
    if (!hasReusableSelection_) {
        return false;
    }
    if (frameState.viewportWidthPixels != lastSelectionViewportWidth_ ||
        frameState.viewportHeightPixels != lastSelectionViewportHeight_) {
        return false;
    }
    if (resourceRevision != lastSelectionResourceRevision_ ||
        overlaySignature != lastSelectionOverlaySignature_) {
        return false;
    }
    if (!selectorViewsEquivalent(
            frameState.selectorViews,
            lastSelectorViews_)) {
        return false;
    }
    if (!tilePlan_.tilesFadingOut.empty() ||
        tilePlan_.fadingNodeCount > 0) {
        return false;
    }
    if (hasTilesetPendingWork() || hasRasterOverlayPendingWork()) {
        return false;
    }
    if (lastRequestIssuedWork_ || lastRequestBlockedByInflight_) {
        return false;
    }
    return true;
}

std::string Tileset::terrainCacheKey(const TileKey& key) const {
    return key.schemeId + "/" + std::to_string(key.z) + "/" +
           std::to_string(key.x) + "/" + std::to_string(key.y);
}

float Tileset::sampleHeight(double lngRad, double latRad) const {
    const DecodedHeightmap* bestHeightmap = nullptr;
    Rectangle bestBounds;
    int bestZoom = -1;

    for (const auto& [ck, tile] : tiles_) {
        if (!tile || tile->key.z < bestZoom ||
            !tile->bounds.contains(lngRad, latRad)) {
            continue;
        }
        auto terrainIt = terrainCache_.find(ck);
        if (terrainIt == terrainCache_.end() || !terrainIt->second ||
            !terrainIt->second->valid()) {
            continue;
        }

        bestHeightmap = terrainIt->second.get();
        bestBounds = tile->bounds;
        bestZoom = tile->key.z;
    }

    if (!bestHeightmap) {
        return 0.0f;
    }
    return sampleHeightFromDecodedTile(
        *bestHeightmap, bestBounds, lngRad, latRad);
}

TilesetTile* Tileset::ensureTile(const TileKey& key) {
    const std::string ck = terrainCacheKey(key);
    const std::optional<TilesetContentTileMetadata> contentMetadata =
        contentProvider_ ? contentProvider_->tileMetadata(key) : std::nullopt;
    auto it = tiles_.find(ck);
    if (it != tiles_.end() && it->second) {
        if (contentMetadata) {
            TilesetTile& existing = *it->second;
            existing.bounds = contentMetadata->bounds;
            existing.geometricError = contentMetadata->geometricError;
            existing.refine = contentMetadata->refine;
            existing.unconditionallyRefine =
                contentMetadata->unconditionallyRefine;
            existing.boundingVolume = contentMetadata->boundingVolume;
            existing.viewerRequestVolume =
                contentMetadata->viewerRequestVolume;
            existing.contentBoundingVolume =
                contentMetadata->contentBoundingVolume;
            if (contentMetadata->boundingVolume &&
                contentMetadata->boundingVolume->kind ==
                    TileBoundingVolumeKind::Region) {
                setTerrainHeightRange(
                    existing,
                    contentMetadata->boundingVolume->minimumHeight,
                    contentMetadata->boundingVolume->maximumHeight);
            }
        }
        return it->second.get();
    }

    TilesetTile* parent = nullptr;
    if (contentMetadata && contentMetadata->parentKey) {
        parent = ensureTile(*contentMetadata->parentKey);
    } else if (!contentMetadata && key.z > 0) {
        parent = ensureTile(TilePlanBuilder::parentKey(key));
    }

    const Rectangle bounds = contentMetadata && contentMetadata->hasExplicitBounds
        ? contentMetadata->bounds
        : tileScheme_->tileToRectangle(key);
    auto tile = std::make_unique<TilesetTile>(
        key, bounds, parent);
    tile->geometricError = contentMetadata
        ? contentMetadata->geometricError
        : cesiumTerrainGeometricError(tile->bounds);
    if (contentMetadata) {
        tile->refine = contentMetadata->refine;
        tile->unconditionallyRefine =
            contentMetadata->unconditionallyRefine;
        tile->boundingVolume = contentMetadata->boundingVolume;
        tile->viewerRequestVolume =
            contentMetadata->viewerRequestVolume;
        tile->contentBoundingVolume =
            contentMetadata->contentBoundingVolume;
    }
    tile->rasterOverlays.resize(rasterOverlays_.size());
    if (tile->boundingVolume &&
        tile->boundingVolume->kind == TileBoundingVolumeKind::Region) {
        setTerrainHeightRange(
            *tile,
            tile->boundingVolume->minimumHeight,
            tile->boundingVolume->maximumHeight);
    } else if (parent) {
        inheritTerrainHeightRange(*tile, *parent);
    } else {
        setDefaultTerrainHeightRange(*tile);
    }

    TilesetTile* raw = tile.get();
    tiles_[ck] = std::move(tile);

    if (parent) {
        auto& children = parent->children;
        if (std::find(children.begin(), children.end(), raw) == children.end()) {
            children.push_back(raw);
        }
    }

    return raw;
}

void Tileset::resetTileSelectionState() {
    for (auto& [ck, tile] : tiles_) {
        if (!tile) continue;
        tile->previousSelectionState = tile->selectionState;
        tile->selectionState = TileSelectionState::NotVisited;
        tile->screenSpaceError = 0.0;
        tile->inFrustum = false;
        tile->cameraInside = false;
        tile->ancestorMeetsSse = false;
        tile->surfaceDrawable = hasSurfaceDrawable(*tile);
        tile->completeRenderable = isTileCompleteRenderable(*tile);
        tile->renderable = tile->completeRenderable;
        (void)ck;
    }
}

bool Tileset::hasSurfaceDrawable(const TilesetTile& tile) const {
    if (tile.contentKind == TileContentKind::Render) {
        return tile.meshReady && tile.gpuVertexBuffer != nullptr;
    }
    if (tile.contentKind == TileContentKind::External ||
        tile.contentKind == TileContentKind::Empty) {
        return tile.loadState == TileLoadState::Done ||
               tile.loadState == TileLoadState::Failed;
    }
    return false;
}

bool Tileset::isTileCompleteRenderable(const TilesetTile& tile) const {
    if (tile.loadState == TileLoadState::Failed) {
        return true;
    }

    if (tile.loadState != TileLoadState::Done) {
        return false;
    }

    if (tile.unconditionallyRefine && !tile.children.empty()) {
        return false;
    }

    for (size_t i = 0; i < rasterOverlays_.size(); ++i) {
        const ActivatedRasterOverlay* activeOverlay = rasterOverlays_[i];
        if (!activeOverlay || !activeOverlay->visible() ||
            !activeOverlay->getOverlay().blocksCompleteRenderable()) {
            continue;
        }
        if (i >= tile.rasterOverlays.size() ||
            !tile.rasterOverlays[i] ||
            tile.rasterOverlays[i]->getReadyTile() == nullptr) {
            return false;
        }
    }

    switch (tile.contentKind) {
        case TileContentKind::Empty:
        case TileContentKind::External:
            return true;
        case TileContentKind::Render:
            return tile.meshReady;
        case TileContentKind::Unknown:
            return false;
    }

    return false;
}

bool Tileset::isTileRenderable(const TilesetTile& tile) const {
    return isTileCompleteRenderable(tile);
}

std::vector<size_t> Tileset::rasterOverlayProcessingOrder() const {
    std::vector<size_t> order;
    order.reserve(rasterOverlays_.size());
    for (size_t i = 0; i < rasterOverlays_.size(); ++i) {
        order.push_back(i);
    }
    std::stable_sort(order.begin(), order.end(), [this](size_t a, size_t b) {
        const ActivatedRasterOverlay* lhs = rasterOverlays_[a];
        const ActivatedRasterOverlay* rhs = rasterOverlays_[b];
        const int lhsPriority = lhs
            ? static_cast<int>(lhs->getOverlay().priority())
            : static_cast<int>(RasterOverlayPriority::Low);
        const int rhsPriority = rhs
            ? static_cast<int>(rhs->getOverlay().priority())
            : static_cast<int>(RasterOverlayPriority::Low);
        return lhsPriority > rhsPriority;
    });
    return order;
}

void Tileset::prepareRasterOverlaysForSelection(TilesetTile& tile) {
    if (tile.loadState != TileLoadState::Done ||
        tile.contentKind != TileContentKind::Render ||
        !tile.meshReady ||
        !tile.mesh) {
        return;
    }

    // The selector uses isTileRenderable() before buildTileDrawCommand() gets a
    // chance to send resource-prep notifications. Advance loaded/ancestor
    // raster mappings here so child coverage decisions are based on current
    // raster state, not on last frame's build phase.
    if (!rasterOverlays_.empty() &&
        tile.rasterOverlays.size() >= rasterOverlays_.size()) {
        bool allRequiredOverlaysReady = true;
        for (size_t i = 0; i < rasterOverlays_.size(); ++i) {
            const ActivatedRasterOverlay* activeOverlay = rasterOverlays_[i];
            if (!activeOverlay || !activeOverlay->visible() ||
                !activeOverlay->getOverlay().blocksCompleteRenderable()) {
                continue;
            }
            const auto& mapped = tile.rasterOverlays[i];
            if (!mapped || !mapped->getReadyTile()) {
                allRequiredOverlaysReady = false;
                break;
            }
        }
        if (allRequiredOverlaysReady) {
            return;
        }
    }
    prefetchRasterOverlays(tile);
}

bool Tileset::hasLoadedTerrainContent(const TilesetTile& tile) const {
    auto it = terrainCache_.find(terrainCacheKey(tile.key));
    return it != terrainCache_.end() && it->second != nullptr;
}

bool Tileset::isAvailabilityBoundaryTile(const TilesetTile& tile) const {
    auto* qmProvider = dynamic_cast<const QuantizedMeshTerrainProvider*>(
        terrainProvider_.get());
    if (!qmProvider) {
        return false;
    }
    return qmProvider->isAvailabilityBoundaryLevel(tile.key.z);
}

const TilesetTile* Tileset::findUpsampleSourceTile(
    const TilesetTile& tile,
    bool allowUnloadingSource) const {
    const TilesetTile* ancestor = tile.parent;
    while (ancestor) {
        const bool sourceStateReady =
            ancestor->loadState == TileLoadState::Done ||
            (allowUnloadingSource &&
             ancestor->loadState == TileLoadState::Unloading);
        if (sourceStateReady &&
            ancestor->contentKind == TileContentKind::Render &&
            ancestor->meshReady &&
            ancestor->mesh) {
            return ancestor;
        }
        ancestor = ancestor->parent;
    }
    return nullptr;
}

bool Tileset::prepareUpsampleSourceTile(TilesetTile& tile, double priority) {
    if (findUpsampleSourceTile(tile)) {
        return true;
    }

    for (TilesetTile* ancestor = tile.parent;
         ancestor;
         ancestor = ancestor->parent) {
        if ((ancestor->loadState == TileLoadState::ContentLoaded ||
             ancestor->loadState == TileLoadState::Done) &&
            ancestor->contentKind == TileContentKind::Render) {
            ensureTileMesh(*ancestor);
            if (findUpsampleSourceTile(tile)) {
                return true;
            }
        }

        if (ancestor->loadState == TileLoadState::Unloaded ||
            ancestor->loadState == TileLoadState::FailedTemporarily) {
            queueTileLoad(ancestor->key, TileLoadPriorityGroup::Urgent, priority);
            return false;
        }

        if (ancestor->loadState == TileLoadState::ContentLoading ||
            ancestor->loadState == TileLoadState::Unloading) {
            return false;
        }
    }

    return false;
}

bool Tileset::wasRenderedLastFrame(const TilesetTile& tile) const {
    return tile.previousSelectionState == TileSelectionState::Rendered;
}

bool Tileset::childWasRefinedLastFrame(const TilesetTile& tile) const {
    for (const TilesetTile* child : tile.children) {
        if (!child) continue;
        if (originalSelectionState(child->previousSelectionState) ==
            TileSelectionState::Refined) {
            return true;
        }
    }
    return false;
}

bool Tileset::anyDescendantWasRenderedLastFrame(const TilesetTile& tile) const {
    for (const TilesetTile* child : tile.children) {
        if (!child) continue;
        if (child->previousSelectionState == TileSelectionState::Rendered ||
            anyDescendantWasRenderedLastFrame(*child)) {
            return true;
        }
    }
    return false;
}

TileOcclusionState Tileset::checkSingleTileOcclusion(
    const TilesetTile& tile) const {
    if (occlusionCallback_) {
        return occlusionCallback_(tile);
    }
    return checkSoftwareOcclusion(tile);
}

TileOcclusionState Tileset::checkOcclusion(const TilesetTile& tile) const {
    const TileOcclusionState tileOcclusion =
        checkSingleTileOcclusion(tile);
    if (tileOcclusion == TileOcclusionState::Occluded ||
        tileOcclusion == TileOcclusionState::OcclusionUnavailable ||
        tile.children.empty()) {
        return tileOcclusion;
    }

    // cesium-native: if the tile itself is not occluded, use the children
    // bounding volumes as a tighter union. Unconditional-refine children are
    // not a reliable finite union, so keep the parent visible.
    for (const TilesetTile* child : tile.children) {
        if (!child || child->unconditionallyRefine) {
            return TileOcclusionState::NotOccluded;
        }
    }

    bool anyUnavailable = false;
    for (const TilesetTile* child : tile.children) {
        const TileOcclusionState childOcclusion =
            checkSingleTileOcclusion(*child);
        if (childOcclusion == TileOcclusionState::NotOccluded) {
            return TileOcclusionState::NotOccluded;
        }
        if (childOcclusion == TileOcclusionState::OcclusionUnavailable) {
            anyUnavailable = true;
        }
    }

    return anyUnavailable
        ? TileOcclusionState::OcclusionUnavailable
        : TileOcclusionState::Occluded;
}

TileOcclusionState Tileset::checkSoftwareOcclusion(
    const TilesetTile& tile) const {
    const auto& ellipsoid = Ellipsoid::WGS84();
    const Cartographic cameraCart =
        ellipsoid.cartesianToCartographic(lastCameraPosition_);
    const bool cameraInsideTileRegion =
        !tile.boundingVolume ||
        tile.boundingVolume->kind == TileBoundingVolumeKind::Region
            ? tile.bounds.contains(cameraCart.longitude(),
                                   cameraCart.latitude())
            : false;
    if (cameraCart.height() <= 0.0 ||
        cameraInsideTileRegion) {
        return TileOcclusionState::NotOccluded;
    }

    const Vec3 cameraScaled(
        lastCameraPosition_.x() / ellipsoid.semiMajorAxis(),
        lastCameraPosition_.y() / ellipsoid.semiMajorAxis(),
        lastCameraPosition_.z() / ellipsoid.semiMinorAxis());
    const double vhMagnitudeSquared = cameraScaled.lengthSquared() - 1.0;
    if (vhMagnitudeSquared <= 0.0) {
        return TileOcclusionState::NotOccluded;
    }

    auto scaledPointOccluded = [&](const Vec3& pointScaled) {
        const Vec3 vt = pointScaled - cameraScaled;
        const double vtMagnitudeSquared = vt.lengthSquared();
        if (vtMagnitudeSquared <= 0.0) {
            return false;
        }
        const double vtDotVc = -vt.dot(cameraScaled);
        return vtDotVc > vhMagnitudeSquared &&
               (vtDotVc * vtDotVc) / vtMagnitudeSquared >
                   vhMagnitudeSquared;
    };

    if (tile.mesh && tile.mesh->hasHorizonOcclusionPoint) {
        return scaledPointOccluded(tile.mesh->horizonOcclusionPoint)
            ? TileOcclusionState::Occluded
            : TileOcclusionState::NotOccluded;
    }

    auto pointOccludedByEllipsoid = [&](const Vec3& pointEcef) {
        const Vec3 ray = pointEcef - lastCameraPosition_;
        const double distance = ray.length();
        if (distance <= 1e-6) {
            return false;
        }
        const Vec3 direction = ray / distance;
        const auto interval =
            ellipsoid.rayIntersectionInterval(lastCameraPosition_, direction);
        if (!interval) {
            return false;
        }
        return interval->entryDistance > 0.0 &&
               interval->entryDistance < distance - 1.0;
    };

    if (tile.boundingVolume &&
        tile.boundingVolume->kind != TileBoundingVolumeKind::Region) {
        std::vector<Vec3> samples;
        if (tile.boundingVolume->kind == TileBoundingVolumeKind::Sphere) {
            const Vec3 center = tile.boundingVolume->sphere.getCenter();
            const double radius = tile.boundingVolume->sphere.getRadius();
            samples = {
                center,
                center + Vec3(radius, 0.0, 0.0),
                center - Vec3(radius, 0.0, 0.0),
                center + Vec3(0.0, radius, 0.0),
                center - Vec3(0.0, radius, 0.0),
                center + Vec3(0.0, 0.0, radius),
                center - Vec3(0.0, 0.0, radius)
            };
        } else {
            const Vec3 center = tile.boundingVolume->box.getCenter();
            const Vec3 axes[3] = {
                tile.boundingVolume->box.getHalfAxis(0),
                tile.boundingVolume->box.getHalfAxis(1),
                tile.boundingVolume->box.getHalfAxis(2)
            };
            samples.push_back(center);
            for (int sx : {-1, 1}) {
                for (int sy : {-1, 1}) {
                    for (int sz : {-1, 1}) {
                        samples.push_back(
                            center +
                            static_cast<double>(sx) * axes[0] +
                            static_cast<double>(sy) * axes[1] +
                            static_cast<double>(sz) * axes[2]);
                    }
                }
            }
        }
        for (const Vec3& sample : samples) {
            if (!pointOccludedByEllipsoid(sample)) {
                return TileOcclusionState::NotOccluded;
            }
        }
        return TileOcclusionState::Occluded;
    }

    const double sampleHeight = std::max(0.0, terrainMaximumHeight(tile));
    const double midLon = tile.bounds.crossesAntimeridian()
        ? std::fmod(tile.bounds.west() + tile.bounds.width() * 0.5 +
                        kPiForLongitudeWrap,
                    kTwoPiForLongitudeWrap) -
              kPiForLongitudeWrap
        : (tile.bounds.west() + tile.bounds.east()) * 0.5;
    const double midLat = (tile.bounds.south() + tile.bounds.north()) * 0.5;
    const double longitudes[3] = {tile.bounds.west(), midLon, tile.bounds.east()};
    const double latitudes[3] = {tile.bounds.south(), midLat, tile.bounds.north()};

    for (double lat : latitudes) {
        for (double lon : longitudes) {
            const Vec3 point = ellipsoid.cartographicToCartesian(
                Cartographic::fromRadians(lon, lat, sampleHeight));
            if (!pointOccludedByEllipsoid(point)) {
                return TileOcclusionState::NotOccluded;
            }
        }
    }

    return TileOcclusionState::Occluded;
}

Tileset::TraversalDetails
Tileset::createTraversalDetailsForSingleTile(const TilesetTile& tile) const {
    const bool renderable = isTileRenderable(tile);
    const TileSelectionState previousState =
        originalSelectionState(tile.previousSelectionState);
    const bool previousRendered =
        previousState == TileSelectionState::Rendered ||
        (previousState == TileSelectionState::Refined &&
         (tile.refine == TileRefine::Add ||
          anyDescendantWasRenderedLastFrame(tile)));

    TraversalDetails details;
    details.allAreRenderable = renderable;
    details.anyWereRenderedLastFrame = renderable && previousRendered;
    details.notYetRenderableCount = renderable ? 0 : 1;
    return details;
}

Tileset::TraversalDetails
Tileset::createTraversalDetailsForCulledTile(const TilesetTile& tile) const {
    if (options_.forbidHoles && tile.refine == TileRefine::Replace) {
        return createTraversalDetailsForSingleTile(tile);
    }
    return TraversalDetails{};
}

void Tileset::queueTileLoad(const TileKey& key,
                            TileLoadPriorityGroup group,
                            double priority) {
    auto it = std::find_if(loadQueue_.begin(), loadQueue_.end(),
        [&key](const TileLoadRequest& request) {
            return request.key == key;
        });
    if (it != loadQueue_.end()) {
        if (static_cast<int>(group) > static_cast<int>(it->group)) {
            it->group = group;
            it->priority = priority;
        } else if (group == it->group) {
            it->priority = std::min(it->priority, priority);
        }
        return;
    }
    loadQueue_.push_back(TileLoadRequest{key, group, priority});
}

void Tileset::addTileToCurrentPlan(TilesetTile& tile,
                                   double tileSse,
                                   bool queueForLoad,
                                   double tilePriority) {
    tile.selectionState = TileSelectionState::Rendered;
    tile.screenSpaceError = tileSse;
    if (!options_.enableLodTransitionPeriod) {
        tile.lodTransitionFadePercentage = 1.0f;
    }
    tilePlan_.visibleTiles.push_back(tile.key);
    if (queueForLoad) {
        queueTileLoad(
            tile.key,
            TileLoadPriorityGroup::Normal,
            tilePriority);
    }
}

bool Tileset::wasRenderedInPreviousSelection(
    const TilesetTile& tile) const {
    return tile.previousSelectionState == TileSelectionState::Rendered ||
           (tile.previousSelectionState == TileSelectionState::Refined &&
            tile.refine == TileRefine::Add);
}

bool Tileset::hasLodTransitionRenderContent(const TilesetTile& tile) const {
    return tile.contentKind == TileContentKind::Render &&
           isTileRenderable(tile);
}

void Tileset::updateLodTransitions(double deltaSeconds) {
    tilePlan_.tilesFadingOut.clear();
    tilePlan_.tileTransitions.clear();
    tilePlan_.fadingNodeCount = 0;

    std::unordered_set<std::string> currentRenderKeys;
    currentRenderKeys.reserve(tilePlan_.visibleTiles.size());
    for (const TileKey& key : tilePlan_.visibleTiles) {
        currentRenderKeys.insert(terrainCacheKey(key));
    }

    if (!options_.enableLodTransitionPeriod) {
        tilesFadingOut_.clear();
        for (const TileKey& key : tilePlan_.visibleTiles) {
            auto it = tiles_.find(terrainCacheKey(key));
            if (it != tiles_.end() && it->second) {
                it->second->lodTransitionFadePercentage = 1.0f;
            }
        }
        return;
    }

    const float transitionDelta = static_cast<float>(
        std::max(0.0, deltaSeconds) /
        std::max(1e-6, static_cast<double>(options_.lodTransitionLength)));

    for (auto& [ck, tile] : tiles_) {
        if (!tile || currentRenderKeys.find(ck) != currentRenderKeys.end()) {
            continue;
        }
        if (!wasRenderedInPreviousSelection(*tile) ||
            !hasLodTransitionRenderContent(*tile)) {
            continue;
        }
        if (tilesFadingOut_.insert(ck).second) {
            tile->lodTransitionFadePercentage = 0.0f;
        }
    }

    std::unordered_set<std::string> returnedFromFadeOut;
    for (auto it = tilesFadingOut_.begin(); it != tilesFadingOut_.end();) {
        if (currentRenderKeys.find(*it) != currentRenderKeys.end()) {
            auto tileIt = tiles_.find(*it);
            if (tileIt != tiles_.end() && tileIt->second) {
                tileIt->second->lodTransitionFadePercentage = 0.0f;
            }
            returnedFromFadeOut.insert(*it);
            it = tilesFadingOut_.erase(it);
            continue;
        }

        auto tileIt = tiles_.find(*it);
        if (tileIt == tiles_.end() || !tileIt->second ||
            !hasLodTransitionRenderContent(*tileIt->second)) {
            it = tilesFadingOut_.erase(it);
            continue;
        }

        TilesetTile& tile = *tileIt->second;
        if (tile.lodTransitionFadePercentage >= 1.0f) {
            tile.lodTransitionFadePercentage = 0.0f;
            it = tilesFadingOut_.erase(it);
            continue;
        }

        tile.lodTransitionFadePercentage = std::min(
            tile.lodTransitionFadePercentage + transitionDelta,
            1.0f);
        const float renderOpacity =
            1.0f - tile.lodTransitionFadePercentage;
        tilePlan_.tilesFadingOut.push_back(TileTransition{
            tile.key,
            renderOpacity,
            1
        });
        tilePlan_.tileTransitions.push_back(TileTransition{
            tile.key,
            renderOpacity,
            1
        });
        if (renderOpacity > 0.001f) {
            ++tilePlan_.fadingNodeCount;
        }
        ++it;
    }

    for (const TileKey& key : tilePlan_.visibleTiles) {
        const std::string ck = terrainCacheKey(key);
        auto tileIt = tiles_.find(ck);
        if (tileIt == tiles_.end() || !tileIt->second) {
            continue;
        }

        TilesetTile& tile = *tileIt->second;
        if (!hasLodTransitionRenderContent(tile)) {
            continue;
        }
        const bool wasFadingOut =
            returnedFromFadeOut.find(ck) != returnedFromFadeOut.end() ||
            tilesFadingOut_.erase(ck) > 0;
        if (wasFadingOut || !wasRenderedInPreviousSelection(tile)) {
            tile.lodTransitionFadePercentage = 0.0f;
        }

        tile.lodTransitionFadePercentage = std::min(
            tile.lodTransitionFadePercentage + transitionDelta,
            1.0f);
        tilePlan_.tileTransitions.push_back(TileTransition{
            tile.key,
            tile.lodTransitionFadePercentage,
            1
        });
        if (tile.lodTransitionFadePercentage < 0.999f) {
            ++tilePlan_.fadingNodeCount;
        }
    }
}

void Tileset::ensureTileChildren(TilesetTile& tile) {
    if (contentProvider_) {
        const std::vector<TileKey> childKeys =
            contentProvider_->childTiles(tile.key);
        if (!childKeys.empty()) {
            for (const TileKey& childKey : childKeys) {
                TilesetTile* child = ensureTile(childKey);
                if (!child) continue;
                child->parent = &tile;
                if (std::find(tile.children.begin(),
                              tile.children.end(),
                              child) == tile.children.end()) {
                    tile.children.push_back(child);
                }
            }
            return;
        }
    }

    if (tile.key.z >= tileScheme_->maxZoom()) return;

    if (tile.upsampledFromParent) return;
    if (!terrainProvider_) return;

    // cesium-native LayerJsonTerrainLoader::createTileChildren:
    // availability-boundary tiles return RetryLater until their own content
    // finishes loading because that content may resolve metadata subtrees.
    if (isAvailabilityBoundaryTile(tile) && !hasLoadedTerrainContent(tile)) {
        return;
    }

    const int childZ = tile.key.z + 1;
    const int childX = tile.key.x * 2;
    const int childY = tile.key.y * 2;

    struct ChildAvailability {
        TileKey key;
        TileAvailabilityState state = TileAvailabilityState::NotAvailable;
    };
    std::vector<ChildAvailability> children;
    children.reserve(4);
    bool anyChildAvailable = false;
    for (int dy = 0; dy < 2; ++dy) {
        for (int dx = 0; dx < 2; ++dx) {
            TileKey childKey{tile.key.schemeId, childZ, childX + dx, childY + dy};
            if (tile.key.schemeId == "Geographic-TMS") {
                if (childKey.x < 0 || childKey.x >= geographicTmsXCount(childZ) ||
                    childKey.y < 0 || childKey.y >= geographicTmsYCount(childZ)) {
                    continue;
                }
            }
            const TileAvailabilityState state =
                terrainProvider_->availabilityState(childKey);
            anyChildAvailable |= state == TileAvailabilityState::Available;
            children.push_back(ChildAvailability{childKey, state});
        }
    }

    // cesium-native LayerJsonTerrainLoader::createTileChildrenImpl:
    // if any child is available, create all children. Non-available children
    // are UpsampledQuadtreeNode equivalents and must not request terrain data.
    if (!anyChildAvailable) return;

    for (const ChildAvailability& childInfo : children) {
        TilesetTile* child = ensureTile(childInfo.key);
        if (!child) continue;
        child->geometricError = tile.geometricError * 0.5;
        if (!child->meshReady) {
            inheritTerrainHeightRange(*child, tile);
        }

        const bool upsampled =
            childInfo.state != TileAvailabilityState::Available;
        if (child->upsampledFromParent != upsampled) {
            child->meshReady = false;
            child->surfaceDrawable = false;
            child->surfaceSource = SurfaceDrawableSource::None;
            child->mesh.reset();
            child->gpuVertexBuffer.reset();
            child->gpuIndexBuffer.reset();
            child->upsampledFromParent = upsampled;
        }
        if (std::find(tile.children.begin(), tile.children.end(), child) ==
            tile.children.end()) {
            tile.children.push_back(child);
        }
    }
}

void Tileset::createRasterOverlayUpsampledChildren(TilesetTile& tile) {
    if (!tile.mesh || tile.children.size() >= 4) {
        return;
    }

    const RasterOverlayDetails& details = tile.mesh->rasterOverlayDetails;
    const Rectangle* subdivisionRectangle = nullptr;
    for (const auto& mapped : tile.rasterOverlays) {
        if (!mapped || !mapped->isMoreDetailAvailable()) {
            continue;
        }
        const RasterOverlayTile* readyTile = mapped->getReadyTile();
        if (!readyTile) {
            continue;
        }
        subdivisionRectangle = details.findRectangleForOverlayProjection(
            readyTile->getTileProvider().getProjection());
        if (subdivisionRectangle) {
            break;
        }
    }

    if (!subdivisionRectangle) {
        return;
    }

    const double centerLng =
        subdivisionRectangle->west() + subdivisionRectangle->width() * 0.5;
    const double centerLat =
        subdivisionRectangle->south() + subdivisionRectangle->height() * 0.5;

    const int childZ = tile.key.z + 1;
    const int childX = tile.key.x * 2;
    const int childY = tile.key.y * 2;
    const std::array<Rectangle, 4> childBounds = {
        Rectangle(
            subdivisionRectangle->west(),
            subdivisionRectangle->south(),
            centerLng,
            centerLat),
        Rectangle(
            centerLng,
            subdivisionRectangle->south(),
            subdivisionRectangle->east(),
            centerLat),
        Rectangle(
            subdivisionRectangle->west(),
            centerLat,
            centerLng,
            subdivisionRectangle->north()),
        Rectangle(
            centerLng,
            centerLat,
            subdivisionRectangle->east(),
            subdivisionRectangle->north())
    };

    tile.refine = TileRefine::Replace;
    if (tile.geometricError <= 0.0) {
        tile.geometricError = cesiumTerrainGeometricError(tile.bounds);
    }

    bool createdOrUpdatedChild = false;
    for (size_t i = 0; i < childBounds.size(); ++i) {
        const int dx = static_cast<int>(i % 2);
        const int dy = static_cast<int>(i / 2);
        TileKey childKey{tile.key.schemeId, childZ, childX + dx, childY + dy};
        TilesetTile* child = ensureTile(childKey);
        if (!child) {
            continue;
        }

        child->parent = &tile;
        child->bounds = childBounds[i];
        child->boundingVolume = TileBoundingVolume::fromRegion(
            childBounds[i],
            terrainMinimumHeight(tile),
            terrainMaximumHeight(tile));
        child->contentBoundingVolume = child->boundingVolume;
        child->geometricError = tile.geometricError * 0.5;
        child->refine = TileRefine::Replace;
        child->upsampledFromParent = true;
        child->unconditionallyRefine = false;
        inheritTerrainHeightRange(*child, tile);

        if (std::find(tile.children.begin(), tile.children.end(), child) ==
            tile.children.end()) {
            tile.children.push_back(child);
            createdOrUpdatedChild = true;
        }
    }
    if (createdOrUpdatedChild) {
        markTileResourcesDirty();
    }
}

bool Tileset::canRefine(const TilesetTile& tile) const {
    if (tile.upsampledFromParent) return false;

    if (!tile.children.empty()) {
        return true;
    }

    if (contentProvider_) {
        if (!contentProvider_->childTiles(tile.key).empty()) {
            return true;
        }
        if (contentProvider_->supportsTile(tile.key)) {
            return false;
        }
    }

    if (isAvailabilityBoundaryTile(tile) && !hasLoadedTerrainContent(tile)) {
        return false;
    }

    if (tile.key.z >= tileScheme_->maxZoom()) {
        return false;
    }

    const int childZ = tile.key.z + 1;
    const int childX = tile.key.x * 2;
    const int childY = tile.key.y * 2;
    for (int dy = 0; dy < 2; ++dy) {
        for (int dx = 0; dx < 2; ++dx) {
            TileKey childKey{tile.key.schemeId, childZ, childX + dx, childY + dy};
            if (tile.key.schemeId == "Geographic-TMS") {
                if (childKey.x < 0 || childKey.x >= geographicTmsXCount(childZ) ||
                    childKey.y < 0 || childKey.y >= geographicTmsYCount(childZ)) {
                    continue;
                }
            }
            if (terrainCache_.count(terrainCacheKey(childKey)) ||
                (terrainProvider_ &&
                 terrainProvider_->availabilityState(childKey) ==
                     TileAvailabilityState::Available)) {
                return true;
            }
        }
    }
    return false;
}

double Tileset::computeTileSse(const TilesetTile& tile,
                               const SelectorFrame& selectorFrame,
                               const std::vector<double>& distances) const {
    if (tile.geometricError <= 0.0) return 0.0;
    double largestSse = 0.0;
    const size_t count = std::min(selectorFrame.views.size(), distances.size());
    for (size_t i = 0; i < count; ++i) {
        double distance = std::max(distances[i], 1e-7);
        const auto& view = selectorFrame.views[i];
        const double viewportHeight =
            std::max(1.0, static_cast<double>(view.viewportHeightPixels));
        const glm::dmat4& projection = view.projectionMatrix.raw();
        glm::dvec4 centerNdc =
            projection * glm::dvec4(0.0, 0.0, -distance, 1.0);
        centerNdc /= centerNdc.w;
        glm::dvec4 errorOffsetNdc =
            projection * glm::dvec4(0.0, tile.geometricError, -distance, 1.0);
        errorOffsetNdc /= errorOffsetNdc.w;
        const double sse =
            std::abs((errorOffsetNdc - centerNdc).y) * viewportHeight * 0.5;
        largestSse = std::max(largestSse, sse);
    }
    return largestSse;
}

Tileset::TraversalDetails Tileset::visitTileIfNeeded(
    TilesetTile& tile,
    const SelectorFrame& selectorFrame,
    uint32_t depth,
    bool ancestorMeetsSse) {
    ++selectedTilesVisited_;

    const Cartographic cameraCart =
        Ellipsoid::WGS84().cartesianToCartographic(lastCameraPosition_);
    const bool underCamera =
        tile.bounds.contains(cameraCart.longitude(), cameraCart.latitude());
    tile.ancestorMeetsSse = ancestorMeetsSse;

    auto boundsVisible = [&](const TilesetTile& candidate) {
        if (options_.renderTilesUnderCamera &&
            candidate.bounds.contains(cameraCart.longitude(), cameraCart.latitude())) {
            return true;
        }
        for (const auto& view : selectorFrame.views) {
            if (tileIntersectsFrustum(candidate, view.frustum)) {
                return true;
            }
        }
        return false;
    };

    bool cullWithChildrenBounds =
        tile.refine == TileRefine::Replace && !tile.children.empty();
    if (cullWithChildrenBounds) {
        for (const TilesetTile* child : tile.children) {
            if (child && child->unconditionallyRefine) {
                cullWithChildrenBounds = false;
                break;
            }
        }
    }

    bool inFrustum = false;
    bool visibleFromCamera = false;
    if (cullWithChildrenBounds) {
        // cesium-native frustumCull(cullWithChildrenBounds): only
        // replace-refined tiles with finite children use children bounds for
        // tighter culling. ADD content and unconditional descendants can
        // extend outside the child union.
        for (const TilesetTile* child : tile.children) {
            if (!child) continue;
            if (boundsVisible(*child)) {
                visibleFromCamera = true;
                for (const auto& view : selectorFrame.views) {
                    inFrustum = inFrustum ||
                                tileIntersectsFrustum(*child, view.frustum);
                }
                break;
            }
        }
    } else {
        visibleFromCamera = boundsVisible(tile);
        for (const auto& view : selectorFrame.views) {
            inFrustum = inFrustum || tileIntersectsFrustum(tile, view.frustum);
        }
    }
    tile.inFrustum = inFrustum;
    tile.cameraInside = underCamera;

    std::vector<double> distances;
    distances.reserve(selectorFrame.views.size());
    for (const auto& view : selectorFrame.views) {
        distances.push_back(
            approximateDistanceToTileBounds(tile, view.position));
    }
    const Vec3 tileCenter = tile.boundingVolume
        ? explicitBoundingVolumeCenter(*tile.boundingVolume)
        : tileBoundsCenter(tile.bounds);
    double tilePriority = std::numeric_limits<double>::max();
    const size_t priorityCount =
        std::min(selectorFrame.views.size(), distances.size());
    for (size_t i = 0; i < priorityCount; ++i) {
        const auto& view = selectorFrame.views[i];
        tilePriority = std::min(
            tilePriority,
            computeTilePriority(
                tileCenter,
                view.position,
                view.direction,
                distances[i]));
    }
    bool culled = false;
    bool shouldVisit = true;
    if (!visibleFromCamera) {
        culled = true;
        if (options_.enableFrustumCulling) {
            shouldVisit = false;
        }
    }

    if (shouldVisit) {
        bool visibleInFog = false;
        const size_t fogCount =
            std::min(distances.size(), selectorFrame.fogDensities.size());
        for (size_t i = 0; i < fogCount; ++i) {
            if (isVisibleInFog(distances[i], selectorFrame.fogDensities[i])) {
                visibleInFog = true;
                break;
            }
        }
        if (!visibleInFog) {
            culled = true;
            if (options_.enableFogCulling) {
                shouldVisit = false;
            }
        }
    }

    if (!shouldVisit) {
        if (!visibleFromCamera) {
            ++selectedTilesCulled_;
        } else {
            ++selectedFogCulled_;
        }
        tile.selectionState = TileSelectionState::Culled;
        if (options_.preloadSiblings || options_.forbidHoles) {
            queueTileLoad(
                tile.key,
                options_.forbidHoles ? TileLoadPriorityGroup::Normal
                                     : TileLoadPriorityGroup::Preload,
                tilePriority);
        }
        return createTraversalDetailsForCulledTile(tile);
    }

    if (culled) {
        ++selectedCulledTilesVisited_;
    }

    if (!viewerRequestVolumeAllowsTile(tile, selectorFrame)) {
        tile.selectionState = TileSelectionState::Culled;
        tile.screenSpaceError = 0.0;
        return TraversalDetails{};
    }

    const double tileSse = computeTileSse(tile, selectorFrame, distances);
    const bool meetsSse = culled
        ? (!options_.enforceCulledScreenSpaceError ||
           tileSse < options_.culledScreenSpaceError)
        : tileSse < options_.maximumScreenSpaceError;
    return visitTile(tile,
                     selectorFrame,
                     depth,
                     meetsSse,
                     ancestorMeetsSse,
                     tilePriority,
                     tileSse);
}

Tileset::TraversalDetails Tileset::visitTile(
    TilesetTile& tile,
    const SelectorFrame& selectorFrame,
    uint32_t depth,
    bool meetsSse,
    bool ancestorMeetsSse,
    double tilePriority,
    double tileSse) {
    (void)depth;
    prepareRasterOverlaysForSelection(tile);
    const bool renderable = isTileRenderable(tile);
    tile.renderable = renderable;

    const bool tileCanRefine = canRefine(tile);
    if (!tileCanRefine) {
        addTileToCurrentPlan(tile, tileSse, true, tilePriority);
        return createTraversalDetailsForSingleTile(tile);
    }

    const bool unconditionallyRefine = tile.unconditionallyRefine;
    const bool refineForSse = !meetsSse && !ancestorMeetsSse;
    bool refine = unconditionallyRefine || refineForSse;
    bool queuedForLoad = false;

    // cesium-native: occlusion can stop or delay refinement before descendant
    // traversal, avoiding child loads that may later prove unnecessary.
    const bool tileLastRefined =
        originalSelectionState(tile.previousSelectionState) ==
        TileSelectionState::Refined;
    const bool shouldCheckOcclusion =
        options_.enableOcclusionCulling &&
        refine &&
        !unconditionallyRefine &&
        (!tileLastRefined || !childWasRefinedLastFrame(tile));
    if (shouldCheckOcclusion) {
        const TileOcclusionState occlusion = checkOcclusion(tile);
        if (occlusion == TileOcclusionState::Occluded) {
            ++selectedTilesOccluded_;
            refine = false;
            meetsSse = true;
        } else if (
            occlusion == TileOcclusionState::OcclusionUnavailable &&
            options_.delayRefinementForOcclusion &&
            originalSelectionState(tile.previousSelectionState) !=
                TileSelectionState::Refined) {
            ++selectedTilesWaitingForOcclusionResults_;
            refine = false;
            meetsSse = true;
        }
    }

    // cesium-native mustContinueRefiningToDeeperTiles:
    // if this tile now meets SSE but is still not renderable, keep the
    // descendants from the previous frame visible while this tile loads.
    if (!refine &&
        originalSelectionState(tile.previousSelectionState) ==
            TileSelectionState::Refined &&
        !renderable) {
        const bool alreadyHasAncestorMeetingSse = ancestorMeetsSse;
        refine = true;
        ancestorMeetsSse = true;
        if (!alreadyHasAncestorMeetingSse) {
            queueTileLoad(
                tile.key,
                TileLoadPriorityGroup::Urgent,
                tilePriority);
            queuedForLoad = true;
        }
    }

    if (!refine) {
        addTileToCurrentPlan(
            tile,
            tileSse,
            !ancestorMeetsSse,
            tilePriority);
        return createTraversalDetailsForSingleTile(tile);
    }

    ensureTileChildren(tile);

    if (tile.refine == TileRefine::Add) {
        addTileToCurrentPlan(tile, tileSse, !queuedForLoad, tilePriority);
        queuedForLoad = true;
    }

    const size_t firstRenderedDescendant = tilePlan_.visibleTiles.size();
    const size_t loadQueueBeforeChildren = loadQueue_.size();

    TraversalDetails traversalDetails;
    for (TilesetTile* child : tile.children) {
        if (!child) continue;
        const TraversalDetails childDetails = visitTileIfNeeded(
            *child,
            selectorFrame,
            depth + 1,
            ancestorMeetsSse);
        traversalDetails.allAreRenderable &=
            childDetails.allAreRenderable;
        traversalDetails.anyWereRenderedLastFrame |=
            childDetails.anyWereRenderedLastFrame;
        traversalDetails.notYetRenderableCount +=
            childDetails.notYetRenderableCount;
    }

    const bool kickDueToNonReadyDescendant =
        !traversalDetails.allAreRenderable &&
        !traversalDetails.anyWereRenderedLastFrame;
    const bool kickDueToTileFadingIn =
        options_.enableLodTransitionPeriod &&
        options_.kickDescendantsWhileFadingIn &&
        originalSelectionState(tile.previousSelectionState) ==
            TileSelectionState::Rendered &&
        hasLodTransitionRenderContent(tile) &&
        tile.lodTransitionFadePercentage < 1.0f;
    const bool kickDueToUnconditionallyRefinedMissingDescendant =
        unconditionallyRefine && !traversalDetails.allAreRenderable;
    const bool willKick =
        (kickDueToNonReadyDescendant ||
         kickDueToTileFadingIn ||
         kickDueToUnconditionallyRefinedMissingDescendant) &&
        (kickDueToUnconditionallyRefinedMissingDescendant ||
         traversalDetails.notYetRenderableCount >
             options_.loadingDescendantLimit ||
         renderable);

    if (willKick) {
        kickVisitedDescendants(tile);
        tilePlan_.visibleTiles.erase(tilePlan_.visibleTiles.begin() +
                                         static_cast<std::ptrdiff_t>(firstRenderedDescendant),
                                     tilePlan_.visibleTiles.end());
        ++selectedTilesKicked_;

        const bool wasReallyRenderedLastFrame =
            wasRenderedLastFrame(tile) && renderable;
        if (!wasReallyRenderedLastFrame &&
            traversalDetails.notYetRenderableCount >
                options_.loadingDescendantLimit &&
            tile.contentKind != TileContentKind::External &&
            !tile.unconditionallyRefine) {
            loadQueue_.resize(loadQueueBeforeChildren);
            if (!queuedForLoad) {
                queueTileLoad(
                    tile.key,
                    TileLoadPriorityGroup::Normal,
                    tilePriority);
                queuedForLoad = true;
            }
        }

        if (tile.refine != TileRefine::Add && renderable) {
            addTileToCurrentPlan(tile, tileSse, false, tilePriority);
        }
        if (!queuedForLoad) {
            queueTileLoad(
                tile.key,
                TileLoadPriorityGroup::Preload,
                tilePriority);
        }
        traversalDetails.allAreRenderable = renderable;
        traversalDetails.anyWereRenderedLastFrame = wasReallyRenderedLastFrame;
        traversalDetails.notYetRenderableCount = renderable ? 0 : 1;
        return traversalDetails;
    }

    tile.selectionState = TileSelectionState::Refined;
    tile.screenSpaceError = tileSse;
    // cesium-native preloadAncestors default: keep ancestors warm for
    // zoom-out and newly exposed areas without competing with urgent/normal
    // tiles needed for the current LOD.
    if (options_.preloadAncestors && !queuedForLoad) {
        queueTileLoad(
            tile.key,
            TileLoadPriorityGroup::Preload,
            tilePriority);
    }
    return traversalDetails;
}

void Tileset::refreshTilePlanRenderEntries() {
    tilePlan_.renderEntries.clear();
    tilePlan_.renderEntryAncestorFallbackCount = 0;
    tilePlan_.renderEntrySynchronousPrepCount = 0;
    tilePlan_.renderEntryDeferredPrepCount = 0;

    std::unordered_set<std::string> renderedGeometryKeys;
    std::unordered_set<std::string> renderedFullGeometryKeys;
    std::unordered_set<std::string> renderedClippedGeometryKeys;

    int renderPrepBudgetRemaining = interactionActiveForFrame_
        ? kActiveInteractionRenderPrepBudget
        : kRecoveryRenderPrepBudget;

    auto findNearestDrawableAncestor = [this](
        TilesetTile& tile) -> TilesetTile* {
        for (TilesetTile* ancestor = tile.parent;
             ancestor;
             ancestor = ancestor->parent) {
            if (hasSurfaceDrawable(*ancestor)) {
                return ancestor;
            }
        }
        return nullptr;
    };

    auto appendRenderEntry = [&](const TileKey& key,
                                 float opacity,
                                 bool selectedThisFrame) {
        TilesetTile* selectedTile = ensureTile(key);
        if (!selectedTile) return;

        TilesetTile* commandTile = selectedTile;
        std::optional<std::array<float, 4>> surfaceClipUv;
        bool usesAncestorFallback = false;

        if (selectedThisFrame &&
            !selectedTile->gltfModel &&
            !hasSurfaceDrawable(*selectedTile)) {
            TilesetTile* drawableAncestor =
                findNearestDrawableAncestor(*selectedTile);
            if (drawableAncestor &&
                (resourceSmoothingActiveForFrame_ ||
                 renderPrepBudgetRemaining <= 0)) {
                commandTile = drawableAncestor;
                surfaceClipUv = clipUvForDescendantBounds(
                    commandTile->bounds,
                    selectedTile->bounds);
                if (!surfaceClipUv) {
                    return;
                }
                usesAncestorFallback = true;
            }
        }

        const std::string selectedCk = terrainCacheKey(selectedTile->key);
        const std::string commandCk = terrainCacheKey(commandTile->key);
        std::string renderDedupKey = commandCk;
        if (surfaceClipUv) {
            if (renderedFullGeometryKeys.count(commandCk) > 0) {
                return;
            }
            renderDedupKey += "|clip:";
            renderDedupKey += selectedCk;
        } else if (renderedClippedGeometryKeys.count(commandCk) > 0) {
            return;
        }
        if (!renderedGeometryKeys.insert(renderDedupKey).second) {
            return;
        }
        if (surfaceClipUv) {
            renderedClippedGeometryKeys.insert(commandCk);
        } else {
            renderedFullGeometryKeys.insert(commandCk);
        }

        bool allowSynchronousMeshPrep = true;
        if (!commandTile->gltfModel && !hasSurfaceDrawable(*commandTile)) {
            if (renderPrepBudgetRemaining > 0) {
                --renderPrepBudgetRemaining;
                ++tilePlan_.renderEntrySynchronousPrepCount;
            } else if (usesAncestorFallback) {
                allowSynchronousMeshPrep = false;
                ++tilePlan_.renderEntryDeferredPrepCount;
            } else {
                // Root/no-ancestor case: allow one direct prep to avoid a blank
                // frame. This path is rare and still bounded by traversal size.
                ++tilePlan_.renderEntrySynchronousPrepCount;
            }
        }

        TileRenderEntry entry;
        entry.selectedKey = selectedTile->key;
        entry.renderKey = commandTile->key;
        entry.opacity = commandTile == selectedTile ? opacity : 1.0f;
        entry.selectedThisFrame = selectedThisFrame;
        entry.usesAncestorFallback = usesAncestorFallback;
        entry.allowSynchronousMeshPrep = allowSynchronousMeshPrep;
        if (surfaceClipUv) {
            entry.surfaceClipEnabled = true;
            entry.surfaceClipUv = *surfaceClipUv;
        }
        if (usesAncestorFallback) {
            ++tilePlan_.renderEntryAncestorFallbackCount;
        }
        tilePlan_.renderEntries.push_back(std::move(entry));
    };

    for (const TileKey& key : tilePlan_.visibleTiles) {
        TilesetTile* tile = ensureTile(key);
        const float transitionOpacity =
            options_.enableLodTransitionPeriod && tile
                ? tile->lodTransitionFadePercentage
                : 1.0f;
        appendRenderEntry(key, transitionOpacity, true);
    }

    for (const TileTransition& transition : tilePlan_.tilesFadingOut) {
        if (transition.opacity <= 0.001f) {
            continue;
        }
        appendRenderEntry(transition.key, transition.opacity, false);
    }
}

Tileset::TilePlanFinalizeTimings
Tileset::finalizeSelectedTilePlan(const FrameState& frameState) {
    TilePlanFinalizeTimings timings;

    const double dedupeStartMs = perf::nowMs();
    std::unordered_set<TileKey> seen;
    std::vector<TileKey> deduped;
    deduped.reserve(tilePlan_.visibleTiles.size());
    for (const TileKey& key : tilePlan_.visibleTiles) {
        if (seen.insert(key).second) {
            deduped.push_back(key);
        }
    }
    tilePlan_.visibleTiles = std::move(deduped);
    timings.dedupeMs = perf::nowMs() - dedupeStartMs;

    const double transitionStartMs = perf::nowMs();
    updateLodTransitions(frameState.deltaSeconds);
    timings.transitionMs = perf::nowMs() - transitionStartMs;

    const double summaryStartMs = perf::nowMs();
    refreshTilePlanRenderEntries();
    if (!tilePlan_.visibleTiles.empty()) {
        tilePlan_.minVisibleZoom = tilePlan_.visibleTiles.front().z;
        tilePlan_.maxVisibleZoom = tilePlan_.visibleTiles.front().z;
        for (const TileKey& key : tilePlan_.visibleTiles) {
            tilePlan_.minVisibleZoom = std::min(tilePlan_.minVisibleZoom, key.z);
            tilePlan_.maxVisibleZoom = std::max(tilePlan_.maxVisibleZoom, key.z);
        }
        tilePlan_.zoom = tilePlan_.maxVisibleZoom;
    }

    for (auto& [ck, tile] : tiles_) {
        if (!tile) continue;
        if (tile->selectionState == TileSelectionState::NotVisited) continue;
        tilePlan_.selectionRecords.push_back(TileSelectionRecord{
            tile->key,
            tile->selectionState,
            tile->previousSelectionState,
            tile->screenSpaceError,
            tile->cameraInside,
            tile->inFrustum,
            tile->ancestorMeetsSse
        });
        if (tile->ancestorMeetsSse) {
            ++tilePlan_.selectionAncestorMeetsSseCount;
        }
        if (selectionWasKicked(tile->selectionState)) {
            ++tilePlan_.selectionKickedCount;
        }
        switch (tile->selectionState) {
            case TileSelectionState::Rendered:
                ++tilePlan_.selectionRenderedCount;
                break;
            case TileSelectionState::Refined:
                ++tilePlan_.selectionRefinedCount;
                break;
            case TileSelectionState::RenderedAndKicked:
            case TileSelectionState::RefinedAndKicked:
            case TileSelectionState::Culled:
            case TileSelectionState::NotVisited:
                break;
        }
        if (tile->cameraInside) ++tilePlan_.cameraInsideNodeCount;
        if (tile->inFrustum) ++tilePlan_.inFrustumNodeCount;
        if (!isTileRenderable(*tile)) ++selectedNotYetRenderable_;
        (void)ck;
    }

    tilePlan_.renderingNodeCount =
        static_cast<int>(tilePlan_.visibleTiles.size());
    tilePlan_.walkthroughNodeCount = tilePlan_.selectionRefinedCount;
    tilePlan_.notRenderingNodeCount = selectedTilesCulled_ + selectedFogCulled_;
    tilePlan_.selectionOccludedCount = selectedTilesOccluded_;
    tilePlan_.selectionWaitingForOcclusionResultsCount =
        selectedTilesWaitingForOcclusionResults_;
    tilePlan_.culledTilesVisitedCount = selectedCulledTilesVisited_;
    tilePlan_.mercatorTileCount = static_cast<int>(tilePlan_.visibleTiles.size());
    timings.summaryMs = perf::nowMs() - summaryStartMs;
    return timings;
}

void Tileset::selectTiles(const FrameState& frameState) {
    const double selectorStartMs = perf::nowMs();
    tilePlan_ = TilePlan{};
    tilePlan_.frameId = frameState.frameId;
    currentFrameTimeSeconds_ = frameState.timeSeconds;
    loadQueue_.clear();
    selectedTilesVisited_ = 0;
    selectedTilesCulled_ = 0;
    selectedTilesKicked_ = 0;
    selectedFogCulled_ = 0;
    selectedNotYetRenderable_ = 0;
    selectedCulledTilesVisited_ = 0;
    selectedTilesOccluded_ = 0;
    selectedTilesWaitingForOcclusionResults_ = 0;

    const size_t tileCountAtSelectorStart = tiles_.size();
    const double resetStartMs = perf::nowMs();
    resetTileSelectionState();
    const double resetMs = perf::nowMs() - resetStartMs;

    const double viewsStartMs = perf::nowMs();
    SelectorFrame selectorFrame;
    selectorFrame.views = buildSelectorViews(frameState);
    selectorFrame.fogDensities.reserve(selectorFrame.views.size());
    for (const auto& view : selectorFrame.views) {
        const double viewHeight = std::max(
            0.0,
            Ellipsoid::WGS84().cartesianToCartographic(view.position).height());
        selectorFrame.fogDensities.push_back(
            computeFogDensity(options_.fogDensityTable, viewHeight));
    }
    const double viewsMs = perf::nowMs() - viewsStartMs;
    if (selectorFrame.views.empty()) {
        return;
    }

    const double traversalStartMs = perf::nowMs();
    std::vector<TileKey> roots =
        contentProvider_ ? contentProvider_->rootTiles() : std::vector<TileKey>{};
    if (!roots.empty()) {
        // cesium-native TilesetJsonLoader supplies explicit roots from the
        // loaded tileset.json rather than deriving roots from a quadtree scheme.
    } else if (tileScheme_->id() == "Geographic-TMS") {
        roots.push_back(TileKey{tileScheme_->id(), 0, 0, 0});
        roots.push_back(TileKey{tileScheme_->id(), 0, 1, 0});
    } else if (tileScheme_->id() == "OpenGlobus-Earth") {
        roots.push_back(TileKey{tileScheme_->id(), 0, 0, 0});
        roots.push_back(TileKey{tileScheme_->id(), 0, 0, 1});
        roots.push_back(TileKey{tileScheme_->id(), 0, 0, 2});
    } else {
        roots.push_back(TileKey{tileScheme_->id(), 0, 0, 0});
    }

    for (const TileKey& key : roots) {
        TilesetTile* root = ensureTile(key);
        if (root) {
            visitTileIfNeeded(*root,
                              selectorFrame,
                              0,
                              false);
        }
    }
    const double traversalMs = perf::nowMs() - traversalStartMs;

    const TilePlanFinalizeTimings finalizeTimings =
        finalizeSelectedTilePlan(frameState);

#ifndef __ANDROID__
    (void)selectorStartMs;
    (void)tileCountAtSelectorStart;
    (void)resetMs;
    (void)viewsMs;
    (void)traversalMs;
    (void)finalizeTimings;
#endif

}

void Tileset::update(const FrameState& frameState) {
    if (!frameState.camera) return;
    const double updateStartMs = perf::nowMs();

    // cesium-native: increment generation each frame so that
    // RenderCommand validator (non-zero check) accepts SurfaceTile commands.
    ++generation_;

    // Track camera movement + direction for priority calculation
    const Vec3 camPos = frameState.camera->position();
    if (lastCameraPosition_.lengthSquared() > 0.0) {
        cameraMoving_ = camPos.distanceTo(lastCameraPosition_) > 2.0;
    }
    lastCameraPosition_ = camPos;
    lastCameraDirection_ = frameState.camera->direction();
    const bool interactionActive =
        frameState.hasInteractionFocus || cameraMoving_;
    interactionActiveForFrame_ = interactionActive;
    if (interactionActive) {
        lastInteractionActiveTimeSeconds_ = frameState.timeSeconds;
    }
    const bool postInteractionSmoothing =
        !interactionActive &&
        lastInteractionActiveTimeSeconds_ >= 0.0 &&
        frameState.timeSeconds - lastInteractionActiveTimeSeconds_ <=
            kPostInteractionResourceSmoothingSeconds;
    resourceSmoothingActiveForFrame_ =
        interactionActive || postInteractionSmoothing;

    FrameResourceBudgetConfig resourceBudgetConfig =
        makeFrameResourceBudgetConfig(
            options_,
            interactionActive,
            resourceSmoothingActiveForFrame_);
    frameResourceBudget_.beginFrame(frameState.frameId, resourceBudgetConfig);

    // Process completed terrain tile requests before selection, matching
    // cesium-native's tileStateUpdater-before-visit flow.
    const double uploadStartMs = perf::nowMs();
    const bool terrainOrContentChanged =
        processPendingUploads(
            interactionActive,
            resourceSmoothingActiveForFrame_,
            &frameResourceBudget_);
    const double uploadMs = perf::nowMs() - uploadStartMs;
    (void)terrainOrContentChanged;

    // cesium-native: process raster overlay tile uploads.
    // Each ActivatedRasterOverlay's provider has a pending upload queue
    // that must be drained on the main thread to create GPU textures.
    // Without this, raster tiles stay in Loading state forever.
    const double rasterUploadStartMs = perf::nowMs();
    int rasterUploadsProcessed = 0;
    for (auto* overlay : rasterOverlays_) {
        if (overlay) {
            rasterUploadsProcessed +=
                overlay->processPendingUploads(interactionActive,
                                               &frameResourceBudget_);
        }
    }
    if (rasterUploadsProcessed > 0) {
        markTileResourcesDirty();
    }
    const double rasterUploadMs = perf::nowMs() - rasterUploadStartMs;

    const uint64_t currentResourceRevision = selectionResourceRevision();
    const uint64_t currentOverlaySignature = overlayConfigurationSignature();
    const bool reusedSelection = canReuseSelection(
        frameState,
        currentResourceRevision,
        currentOverlaySignature);

    // cesium-native selector: cull + SSE + renderability + load queue.
    const double computeStartMs = perf::nowMs();
    if (reusedSelection) {
        tilePlan_.frameId = frameState.frameId;
        loadQueue_.clear();
        selectedTilesVisited_ = 0;
        selectedTilesCulled_ = 0;
        selectedTilesKicked_ = 0;
        selectedFogCulled_ = 0;
        selectedNotYetRenderable_ = 0;
        selectedCulledTilesVisited_ = 0;
        selectedTilesOccluded_ = 0;
        selectedTilesWaitingForOcclusionResults_ = 0;
        refreshTilePlanRenderEntries();
    } else {
        selectTiles(frameState);
        hasReusableSelection_ = true;
        lastSelectionResourceRevision_ = selectionResourceRevision();
        lastSelectionOverlaySignature_ = currentOverlaySignature;
        lastSelectionViewportWidth_ = frameState.viewportWidthPixels;
        lastSelectionViewportHeight_ = frameState.viewportHeightPixels;
        lastSelectorViews_ = frameState.selectorViews;
    }
    const double computeMs = perf::nowMs() - computeStartMs;

    const double prefetchStartMs = perf::nowMs();
    if (!reusedSelection) {
        for (const TileKey& key : tilePlan_.visibleTiles) {
            if (TilesetTile* tile = ensureTile(key)) {
                prefetchRasterOverlays(*tile);
            }
        }
        for (const TileLoadRequest& request : loadQueue_) {
            if (TilesetTile* tile = ensureTile(request.key)) {
                prefetchRasterOverlays(*tile);
            }
        }
    }
    const double prefetchMs = perf::nowMs() - prefetchStartMs;

    // Request the selector's load queue, not the render list. Descendants may
    // continue loading while a renderable ancestor is selected.
    const double requestStartMs = perf::nowMs();
    RequestOutcome requestOutcome;
    if (!reusedSelection) {
        requestOutcome = requestMissingTiles(loadQueue_, &frameResourceBudget_);
    }
    lastRequestIssuedWork_ = requestOutcome.issued > 0;
    lastRequestBlockedByInflight_ = requestOutcome.blockedByInflight;
    const double requestMs = perf::nowMs() - requestStartMs;

    char detail[384];
    std::snprintf(detail, sizeof(detail),
        "render=%zu load=%zu selector=%.2f prefetch=%.2f request=%.2f terrainUpload=%.2f rasterUpload=%.2f cache=%zu pending=%zu visited=%d culled=%d culledVisited=%d fog=%d occluded=%d occWait=%d kicked=%d notReady=%d reused=%d rasterUploads=%d interaction=%d smoothing=%d",
        tilePlan_.visibleTiles.size(),
        loadQueue_.size(),
        computeMs,
        prefetchMs,
        requestMs,
        uploadMs,
        rasterUploadMs,
        terrainCache_.size(),
        pendingRequests_.size(),
        selectedTilesVisited_,
        selectedTilesCulled_,
        selectedCulledTilesVisited_,
        selectedFogCulled_,
        selectedTilesOccluded_,
        selectedTilesWaitingForOcclusionResults_,
        selectedTilesKicked_,
        selectedNotYetRenderable_,
        reusedSelection ? 1 : 0,
        rasterUploadsProcessed,
        interactionActive ? 1 : 0,
        resourceSmoothingActiveForFrame_ ? 1 : 0);
    perf::logTimingAtLeast(frameState.frameId,
                           "Tileset.update",
                           perf::nowMs() - updateStartMs,
                           10.0,
                           detail);
}

Tileset::RequestOutcome Tileset::requestMissingTiles(
    const std::vector<TileLoadRequest>& loadRequests,
    FrameResourceBudget* budget) {
    FrameResourceBudget localBudget;
    if (!budget) {
        FrameResourceBudgetConfig config =
            makeFrameResourceBudgetConfig(options_, false, false);
        localBudget.beginFrame(frameNumber_, config);
        budget = &localBudget;
    }
    RequestOutcome outcome;
    const auto toFramePriority = [](TileLoadPriorityGroup group) {
        switch (group) {
            case TileLoadPriorityGroup::Preload:
                return FrameResourcePriority::Preload;
            case TileLoadPriorityGroup::Normal:
                return FrameResourcePriority::Normal;
            case TileLoadPriorityGroup::Urgent:
                return FrameResourcePriority::Urgent;
        }
        return FrameResourcePriority::Normal;
    };

    std::vector<TileLoadRequest> sorted = loadRequests;
    // cesium-native: higher priority group first; lower numeric priority wins
    // within a group.
    std::sort(sorted.begin(), sorted.end(),
        [](const auto& a, const auto& b) {
            if (a.group != b.group) {
                return static_cast<int>(a.group) > static_cast<int>(b.group);
            }
            return a.priority < b.priority;
        });

    for (const auto& tp : sorted) {
        const TileKey& key = tp.key;
        {
            std::lock_guard<std::mutex> lock(pendingMutex_);
            if (destroying_) {
                break;
            }
            if (!budget->hasNetworkInflightCapacity(
                    static_cast<uint32_t>(pendingRequests_.size()))) {
                outcome.blockedByInflight = true;
                break;
            }
        }

        TileKey requestKey = key;
        const std::string ck = terrainCacheKey(requestKey);
        auto tileStateIt = tiles_.find(ck);
        TilesetTile* tileState =
            tileStateIt != tiles_.end() ? tileStateIt->second.get() : nullptr;
        const bool isUpsampledTile =
            tileState != nullptr && tileState->upsampledFromParent;
        const bool hasTileContent =
            !isUpsampledTile &&
            contentProvider_ &&
            contentProvider_->supportsTile(requestKey);

        if (isUpsampledTile) {
            if (!prepareUpsampleSourceTile(*tileState, tp.priority)) {
                continue;
            }
        } else {
            if (hasTileContent) {
                if (tileState && tileState->contentKind == TileContentKind::Render &&
                    tileState->gltfModel) {
                    continue;
                }
            } else {
                if (terrainCache_.count(ck)) {
                    continue;
                }
                if (!terrainProvider_ ||
                    !terrainProvider_->supportsTile(requestKey)) {
                    continue;
                }
            }
        }

        if (tileState) {
            const TileLoadState state = tileState->loadState;
            if (state != TileLoadState::Unloaded &&
                state != TileLoadState::FailedTemporarily) {
                continue;
            }
        }

        if (isUpsampledTile) {
            {
                std::lock_guard<std::mutex> lock(pendingMutex_);
                if (destroying_) {
                    break;
                }
                if (pendingRequests_.count(ck)) {
                    continue;
                }
                if (pendingUploadKeys_.count(ck)) {
                    continue;
                }
                if (!budget->tryIssue(FrameResourceLane::TerrainRequest,
                                      toFramePriority(tp.group))) {
                    break;
                }
                pendingUploadKeys_.insert(ck);
                pendingUploads_.push_back(
                    PendingTerrainUpload{
                        requestKey,
                        ck,
                        tp.group,
                        tp.priority,
                        nullptr});
            }
            tileState->loadState = TileLoadState::ContentLoading;
            tileState->contentKind = TileContentKind::Unknown;
            ++outcome.issued;
            continue;
        }

        if (hasTileContent) {
            CancellationToken token;
            {
                std::lock_guard<std::mutex> lock(pendingMutex_);
                if (destroying_) {
                    break;
                }
                if (pendingRequests_.count(ck)) {
                    continue;
                }
                if (pendingContentUploadKeys_.count(ck)) {
                    continue;
                }
                if (emptyTiles_.count(ck)) {
                    continue;
                }
                if (!budget->tryIssue(FrameResourceLane::ContentRequest,
                                      toFramePriority(tp.group))) {
                    break;
                }
                pendingRequests_.insert(ck);
                pendingContentRequestKeys_.insert(ck);
                pendingRequestTokens_[ck] = token;
            }
            if (TilesetTile* tile = ensureTile(requestKey)) {
                tile->loadState = TileLoadState::ContentLoading;
                tile->contentKind = TileContentKind::Unknown;
            }
            ++outcome.issued;

            auto* provider = contentProvider_.get();
            provider->requestTileContent(requestKey, token,
                [this,
                 ck,
                 requestKey,
                 token,
                 group = tp.group,
                 priority = tp.priority](
                    const TileKey&, TileContentLoadResult result) mutable {
                    {
                        std::lock_guard<std::mutex> lock(pendingMutex_);
                        if (!destroying_ && !token.isCancelled()) {
                            if (result.status == TileContentLoadStatus::Render &&
                                result.gltfModel) {
                                pendingContentUploadKeys_.insert(ck);
                                pendingContentUploads_.push_back(
                                    PendingContentUpload{
                                        requestKey,
                                        ck,
                                        group,
                                        priority,
                                        std::move(result)});
                            } else {
                                pendingContentTerminalResults_.push_back(
                                    PendingContentTerminalResult{
                                        requestKey,
                                        ck,
                                        group,
                                        priority,
                                        result.status});
                            }
                        }
                        pendingRequests_.erase(ck);
                        pendingContentRequestKeys_.erase(ck);
                        pendingRequestTokens_.erase(ck);
                    }
                    pendingCondition_.notify_all();
                });
            continue;
        }

        CancellationToken token;
        {
            std::lock_guard<std::mutex> lock(pendingMutex_);
            if (destroying_) {
                break;
            }
            if (pendingRequests_.count(ck)) {
                continue;
            }
            if (pendingUploadKeys_.count(ck)) {
                continue;
            }
            if (pendingContentUploadKeys_.count(ck)) {
                continue;
            }
            if (emptyTiles_.count(ck)) {
                continue;
            }
            if (!budget->tryIssue(FrameResourceLane::TerrainRequest,
                                  toFramePriority(tp.group))) {
                break;
            }
            pendingRequests_.insert(ck);
            pendingRequestTokens_[ck] = token;
        }
        if (TilesetTile* tile = ensureTile(requestKey)) {
            tile->loadState = TileLoadState::ContentLoading;
            tile->contentKind = TileContentKind::Unknown;
        }
        ++outcome.issued;

        auto* provider = terrainProvider_.get();
        if (!provider) {
            continue;
        }
        provider->requestTile(requestKey, token,
            [this,
             ck,
             requestKey,
             token,
             group = tp.group,
             priority = tp.priority](const TileKey&, TerrainTileLoadResult result) mutable {
                {
                    std::lock_guard<std::mutex> lock(pendingMutex_);
                    if (!destroying_ && !token.isCancelled()) {
                        if (result.status == TerrainTileLoadStatus::Success &&
                            result.heightmap) {
                            pendingUploadKeys_.insert(ck);
                            pendingUploads_.push_back(
                                PendingTerrainUpload{
                                    requestKey,
                                    ck,
                                    group,
                                    priority,
                                    std::move(result.heightmap)});
                        } else {
                            pendingTerminalResults_.push_back(
                                PendingTerrainTerminalResult{
                                    requestKey,
                                    ck,
                                    group,
                                    priority,
                                    result.status});
                        }
                    }
                    pendingRequests_.erase(ck);
                    pendingRequestTokens_.erase(ck);
                }
                pendingCondition_.notify_all();
            });
    }

    return outcome;
}

void Tileset::prefetchRasterOverlays(TilesetTile& tile) {
    if (rasterOverlays_.empty()) {
        return;
    }

    if (tile.rasterOverlays.size() < rasterOverlays_.size()) {
        tile.rasterOverlays.resize(rasterOverlays_.size());
    }

    const bool hasRenderContentDetails =
        tile.contentKind == TileContentKind::Render && tile.mesh != nullptr;
    const RasterOverlayDetails* renderDetails = hasRenderContentDetails
        ? &tile.mesh->rasterOverlayDetails
        : nullptr;
    std::optional<Rectangle> boundingRegionRectangle;
    if (tile.boundingVolume &&
        tile.boundingVolume->kind == TileBoundingVolumeKind::Region) {
        boundingRegionRectangle = tile.boundingVolume->region;
    }
    const RasterOverlayDetails emptyDetails;
    const RasterOverlayDetails& overlayDetails =
        renderDetails ? *renderDetails : emptyDetails;
    const std::vector<size_t> overlayOrder = rasterOverlayProcessingOrder();
    for (size_t i : overlayOrder) {
        if (i >= tile.rasterOverlays.size()) {
            continue;
        }
        ActivatedRasterOverlay* activeOverlay = rasterOverlays_[i];
        if (!activeOverlay || !activeOverlay->visible()) {
            continue;
        }

        RasterOverlayTileProvider* activeProvider =
            activeOverlay->ensureTileProvider(device_);
        if (!activeProvider) {
            continue;
        }
        const RasterOverlayProjection projection =
            activeProvider->getProjection();
        const Rectangle* geometryRectangle = renderDetails
            ? renderDetails->findRectangleForOverlayProjection(projection)
            : nullptr;
        const Rectangle& rasterTargetRectangle = geometryRectangle
            ? *geometryRectangle
            : (boundingRegionRectangle ? *boundingRegionRectangle : tile.bounds);
        const RasterTargetScreenPixels rasterScreenPixels =
            computeDesiredRasterScreenPixels(
                rasterTargetRectangle,
                tile.geometricError,
                options_.maximumScreenSpaceError);

        auto& mapped = tile.rasterOverlays[i];
        if (!mapped) {
            mapped = std::make_unique<RasterMappedToTilesetTile>();
        }

        RasterOverlayTile* loadingTile = mapped->getLoadingTile();
        if (loadingTile &&
            loadingTile->getState() != RasterOverlayTile::LoadState::Placeholder) {
            if (loadingTile->getState() == RasterOverlayTile::LoadState::Unloaded ||
                loadingTile->getState() == RasterOverlayTile::LoadState::Loading) {
                mapped->loadThrottled(*activeProvider, &frameResourceBudget_);
                continue;
            }
        }
        if (!loadingTile && mapped->getReadyTile()) {
            continue;
        }

        std::vector<RasterOverlayProjection> ignoredMissingProjections;
        mapped->update(
            tile.key,
            overlayDetails,
            rasterScreenPixels.x,
            rasterScreenPixels.y,
            *activeProvider,
            nullptr,
            ignoredMissingProjections,
            tile.parent,
            i,
            tile.boundingVolume ? &*tile.boundingVolume : nullptr,
            hasRenderContentDetails);
        mapped->loadThrottled(*activeProvider, &frameResourceBudget_);
    }
}

bool Tileset::processPendingUploads(bool interactionActive,
                                    bool resourceSmoothingActive,
                                    FrameResourceBudget* budget) {
    FrameResourceBudget localBudget;
    if (!budget) {
        FrameResourceBudgetConfig config;
        config.maxMainThreadFinalizesPerFrame =
            resourceSmoothingActive
                ? static_cast<uint32_t>(kSmoothedMainThreadUploadLimit)
                : options_.maximumSimultaneousTileLoads;
        config.mainThreadTimeMs = options_.mainThreadLoadingTimeLimit;
        config.interactionActive = interactionActive;
        config.smoothingActive = resourceSmoothingActive;
        localBudget.beginFrame(frameNumber_, config);
        budget = &localBudget;
    }
    bool changed = false;
    const auto hasHigherPriority =
        [](TileLoadPriorityGroup lhsGroup,
           double lhsPriority,
           TileLoadPriorityGroup rhsGroup,
           double rhsPriority) {
        if (lhsGroup != rhsGroup) {
            return static_cast<int>(lhsGroup) > static_cast<int>(rhsGroup);
        }
        return lhsPriority < rhsPriority;
    };
    const auto toFramePriority = [](TileLoadPriorityGroup group) {
        switch (group) {
            case TileLoadPriorityGroup::Preload:
                return FrameResourcePriority::Preload;
            case TileLoadPriorityGroup::Normal:
                return FrameResourcePriority::Normal;
            case TileLoadPriorityGroup::Urgent:
                return FrameResourcePriority::Urgent;
        }
        return FrameResourcePriority::Normal;
    };

    auto processTerminalResult =
        [this](const PendingTerrainTerminalResult& result) {
        TilesetTile* tile = ensureTile(result.key);
        if (!tile) return;

        switch (result.status) {
            case TerrainTileLoadStatus::Empty: {
                emptyTiles_.insert(result.cacheKey);
                tile->contentKind = TileContentKind::Empty;
                tile->loadState = TileLoadState::ContentLoaded;

                const TilesetTile* ancestor = tile->parent;
                while (ancestor && ancestor->unconditionallyRefine) {
                    ancestor = ancestor->parent;
                }
                const double parentError = ancestor
                    ? ancestor->geometricError
                    : tile->geometricError * 2.0;
                if (tile->geometricError >= parentError) {
                    tile->unconditionallyRefine = true;
                }
                tile->loadState = TileLoadState::Done;
                markTileResourcesDirty();
                break;
            }
            case TerrainTileLoadStatus::RetryLater:
            case TerrainTileLoadStatus::Cancelled:
                tile->contentKind = TileContentKind::Unknown;
                tile->loadState = TileLoadState::FailedTemporarily;
                markTileResourcesDirty();
                break;
            case TerrainTileLoadStatus::Failed:
            case TerrainTileLoadStatus::Success:
                tile->contentKind = TileContentKind::Unknown;
                tile->loadState = TileLoadState::Failed;
                markTileResourcesDirty();
                break;
        }
    };

    auto processUpload = [this, resourceSmoothingActive](
        PendingTerrainUpload& upload) {
        if (upload.heightmap) {
            if (auto* qmProvider = dynamic_cast<QuantizedMeshTerrainProvider*>(
                    terrainProvider_.get())) {
                qmProvider->applyAvailabilityUpdates(*upload.heightmap);
            }
            ingestQuantizedMeshAvailability(upload.key, *upload.heightmap);
            terrainCache_[upload.cacheKey] = std::move(upload.heightmap);
        }
        if (TilesetTile* tile = ensureTile(upload.key)) {
            tile->loadState = TileLoadState::ContentLoaded;
            tile->contentKind = TileContentKind::Render;
            if (!resourceSmoothingActive) {
                ensureTileMesh(*tile);
            }
            if (!resourceSmoothingActive && !tile->meshReady) {
                tile->contentKind = TileContentKind::Unknown;
                tile->loadState = TileLoadState::FailedTemporarily;
            }
            markTileResourcesDirty();
        }
        std::lock_guard<std::mutex> lock(pendingMutex_);
        pendingUploadKeys_.erase(upload.cacheKey);
    };

    auto processContentTerminalResult =
        [this](const PendingContentTerminalResult& result) {
        TilesetTile* tile = ensureTile(result.key);
        if (!tile) return;

        switch (result.status) {
            case TileContentLoadStatus::Empty:
                emptyTiles_.insert(result.cacheKey);
                tile->contentKind = TileContentKind::Empty;
                tile->loadState = TileLoadState::Done;
                markTileResourcesDirty();
                break;
            case TileContentLoadStatus::External:
                tile->contentKind = TileContentKind::External;
                tile->unconditionallyRefine = true;
                tile->loadState = TileLoadState::Done;
                ensureTileChildren(*tile);
                markTileResourcesDirty();
                break;
            case TileContentLoadStatus::RetryLater:
            case TileContentLoadStatus::Cancelled:
                tile->contentKind = TileContentKind::Unknown;
                tile->loadState = TileLoadState::FailedTemporarily;
                markTileResourcesDirty();
                break;
            case TileContentLoadStatus::Failed:
            case TileContentLoadStatus::Render:
                tile->contentKind = TileContentKind::Unknown;
                tile->loadState = TileLoadState::Failed;
                markTileResourcesDirty();
                break;
        }
    };

    auto processContentUpload = [this](PendingContentUpload& upload) {
        TilesetTile* tile = ensureTile(upload.key);
        if (!tile) {
            std::lock_guard<std::mutex> lock(pendingMutex_);
            pendingContentUploadKeys_.erase(upload.cacheKey);
            return;
        }

        terrainCache_.erase(upload.cacheKey);
        tile->heightmap.reset();
        tile->mesh.reset();
        tile->gpuVertexBuffer.reset();
        tile->gpuIndexBuffer.reset();
        tile->gltfTextureResources.clear();
        tile->gltfPrimitiveResources.clear();
        tile->gltfModel = std::move(upload.result.gltfModel);
        tile->gltfContentTransform = upload.result.contentTransform;
        tile->meshReady = false;
        tile->surfaceDrawable = false;
        tile->surfaceSource = SurfaceDrawableSource::GltfContent;
        tile->contentKind = TileContentKind::Render;
        tile->loadState = TileLoadState::ContentLoaded;
        ensureGltfRenderResources(*tile);
        if (!tile->meshReady) {
            tile->gltfModel.reset();
            tile->gltfTextureResources.clear();
            tile->gltfPrimitiveResources.clear();
            tile->contentKind = TileContentKind::Unknown;
            tile->loadState = TileLoadState::FailedTemporarily;
        }
        markTileResourcesDirty();

        std::lock_guard<std::mutex> lock(pendingMutex_);
        pendingContentUploadKeys_.erase(upload.cacheKey);
    };

    // cesium-native: the load-result continuation updates terminal tile states
    // on the main thread before budgeted render-resource finalization.
    while (true) {
        std::optional<PendingTerrainTerminalResult> terminalResult;
        {
            std::lock_guard<std::mutex> lock(pendingMutex_);
            if (pendingTerminalResults_.empty()) {
                break;
            }

            auto bestIt = pendingTerminalResults_.begin();
            for (auto it = std::next(pendingTerminalResults_.begin());
                 it != pendingTerminalResults_.end();
                 ++it) {
                if (hasHigherPriority(
                        it->group,
                        it->priority,
                        bestIt->group,
                        bestIt->priority)) {
                    bestIt = it;
                }
            }

            terminalResult.emplace(std::move(*bestIt));
            pendingTerminalResults_.erase(bestIt);
        }

        processTerminalResult(*terminalResult);
        changed = true;
    }

    while (true) {
        std::optional<PendingContentTerminalResult> terminalResult;
        {
            std::lock_guard<std::mutex> lock(pendingMutex_);
            if (pendingContentTerminalResults_.empty()) {
                break;
            }

            auto bestIt = pendingContentTerminalResults_.begin();
            for (auto it = std::next(pendingContentTerminalResults_.begin());
                 it != pendingContentTerminalResults_.end();
                 ++it) {
                if (hasHigherPriority(
                        it->group,
                        it->priority,
                        bestIt->group,
                        bestIt->priority)) {
                    bestIt = it;
                }
            }

            terminalResult.emplace(std::move(*bestIt));
            pendingContentTerminalResults_.erase(bestIt);
        }

        processContentTerminalResult(*terminalResult);
        changed = true;
    }

    // cesium-native uses one main-thread load queue. Terrain finalization and
    // glTF/content GPU upload must compete under the same priority order and
    // the same time budget.
    while (true) {
        std::optional<PendingTerrainUpload> terrainUpload;
        std::optional<PendingContentUpload> contentUpload;

        {
            std::lock_guard<std::mutex> lock(pendingMutex_);
            if (pendingUploads_.empty() && pendingContentUploads_.empty()) {
                break;
            }

            auto bestTerrainIt = pendingUploads_.end();
            if (!pendingUploads_.empty()) {
                for (auto it = pendingUploads_.begin();
                     it != pendingUploads_.end();
                     ++it) {
                    if (interactionActive &&
                        it->group != TileLoadPriorityGroup::Urgent) {
                        continue;
                    }
                    if (bestTerrainIt == pendingUploads_.end()) {
                        bestTerrainIt = it;
                        continue;
                    }
                    if (hasHigherPriority(
                            it->group,
                            it->priority,
                            bestTerrainIt->group,
                            bestTerrainIt->priority)) {
                        bestTerrainIt = it;
                    }
                }
            }

            auto bestContentIt = pendingContentUploads_.end();
            if (!pendingContentUploads_.empty()) {
                for (auto it = pendingContentUploads_.begin();
                     it != pendingContentUploads_.end();
                     ++it) {
                    if (interactionActive &&
                        it->group != TileLoadPriorityGroup::Urgent) {
                        continue;
                    }
                    if (bestContentIt == pendingContentUploads_.end()) {
                        bestContentIt = it;
                        continue;
                    }
                    if (hasHigherPriority(
                            it->group,
                            it->priority,
                            bestContentIt->group,
                            bestContentIt->priority)) {
                        bestContentIt = it;
                    }
                }
            }

            const bool useContent =
                bestContentIt != pendingContentUploads_.end() &&
                (bestTerrainIt == pendingUploads_.end() ||
                 hasHigherPriority(
                     bestContentIt->group,
                     bestContentIt->priority,
                     bestTerrainIt->group,
                     bestTerrainIt->priority));
            if (useContent) {
                if (!budget->tryFinalize(
                        FrameResourceLane::ContentFinalize,
                        toFramePriority(bestContentIt->group))) {
                    break;
                }
                contentUpload.emplace(std::move(*bestContentIt));
                pendingContentUploads_.erase(bestContentIt);
            } else if (bestTerrainIt != pendingUploads_.end()) {
                if (!budget->tryFinalize(
                        FrameResourceLane::TerrainFinalize,
                        toFramePriority(bestTerrainIt->group))) {
                    break;
                }
                terrainUpload.emplace(std::move(*bestTerrainIt));
                pendingUploads_.erase(bestTerrainIt);
            }
        }

        const double finalizeStartMs = perf::nowMs();
        if (contentUpload) {
            processContentUpload(*contentUpload);
        } else if (terrainUpload) {
            processUpload(*terrainUpload);
        } else {
            break;
        }
        changed = true;
        budget->recordElapsed(
            contentUpload ? FrameResourceLane::ContentFinalize
                          : FrameResourceLane::TerrainFinalize,
            perf::nowMs() - finalizeStartMs);
    }
    return changed;
}

void Tileset::ingestQuantizedMeshAvailability(
    const TileKey& key,
    DecodedHeightmap& heightmap) {
    if (heightmap.metadataAvailabilityProcessed || heightmap.rawData.empty()) {
        return;
    }
    heightmap.metadataAvailabilityProcessed = true;

    auto* qmProvider = dynamic_cast<QuantizedMeshTerrainProvider*>(
        terrainProvider_.get());
    if (!qmProvider) {
        return;
    }

    if (!qmProvider->isAvailabilityBoundaryLevel(key.z)) {
        return;
    }

    const std::vector<std::array<int, 5>> metadataAvailability =
        QuantizedMeshParser::parseMetadataAvailability(
            heightmap.rawData.data(),
            heightmap.rawData.size());

    for (const auto& r : metadataAvailability) {
        int absLevel = key.z + 1 + r[0];
        if (absLevel >= 0) {
            qmProvider->addAvailabilityRectsForTile(
                key,
                absLevel, {{r[1], r[2], r[3], r[4]}});
        }
    }

    // cesium-native addRectangleAvailabilityToLayer marks the subtree loaded
    // even when metadata has no availability rectangles.
    qmProvider->markSubtreeLoadedForTile(key);
}

void Tileset::ensureTileMesh(TilesetTile& tile) {
    auto it = terrainCache_.find(terrainCacheKey(tile.key));
    const bool hasOwnTerrain = it != terrainCache_.end() && it->second;
    DecodedHeightmap* ownHeightmap = hasOwnTerrain ? it->second.get() : nullptr;

    if (tile.meshReady) {
        if (hasOwnTerrain &&
            tile.surfaceSource != SurfaceDrawableSource::OwnTerrain) {
            tile.meshReady = false;
            tile.surfaceDrawable = false;
            tile.surfaceSource = SurfaceDrawableSource::None;
            tile.mesh.reset();
            tile.gpuVertexBuffer.reset();
            tile.gpuIndexBuffer.reset();
        } else {
            tile.surfaceDrawable = hasSurfaceDrawable(tile);
            if (tile.contentKind == TileContentKind::Render &&
                tile.loadState == TileLoadState::ContentLoaded) {
                tile.loadState = TileLoadState::Done;
            }
            return;
        }
    }

    if (ownHeightmap) {
        ingestQuantizedMeshAvailability(tile.key, *ownHeightmap);
    }

    SurfaceDrawableSource meshSource = SurfaceDrawableSource::None;

    if (!tile.mesh && !hasOwnTerrain) {
        if (!findUpsampleSourceTile(tile, true) && tile.parent) {
            ensureTileMesh(*tile.parent);
        }
        if (const TilesetTile* source = findUpsampleSourceTile(tile, true)) {
            if (source->mesh) {
                auto childMesh = TileSurface::upsampleChildMeshFromParent(
                    *source->mesh,
                    source->bounds,
                    tile.bounds);
                if (childMesh) {
                    tile.mesh = std::make_unique<SurfaceTileMesh>(
                        std::move(*childMesh));
                    meshSource = SurfaceDrawableSource::AncestorUpsample;
                }
            }
        }
    }

    if (!tile.mesh && hasOwnTerrain && ownHeightmap->surfaceMesh) {
        tile.mesh = std::move(ownHeightmap->surfaceMesh);
        meshSource = SurfaceDrawableSource::OwnTerrain;
    }

    if (!tile.mesh && hasOwnTerrain && !ownHeightmap->rawData.empty()) {
        tile.mesh = QuantizedMeshParser::parseToSurfaceTileMesh(
            ownHeightmap->rawData.data(), ownHeightmap->rawData.size(), tile.bounds);
        if (tile.mesh) {
            meshSource = SurfaceDrawableSource::OwnTerrain;
        }
    }

    if (!tile.mesh) {
        tile.mesh = std::make_unique<SurfaceTileMesh>();
        *tile.mesh = TileSurface::buildEllipsoidMesh(
            tile.bounds,
            ownHeightmap ? 64 : 16);
        meshSource = ownHeightmap
            ? SurfaceDrawableSource::OwnTerrain
            : SurfaceDrawableSource::EllipsoidFallback;
        if (ownHeightmap && ownHeightmap->valid()) {
            const auto& ellipsoid = Ellipsoid::WGS84();
            for (auto& v : tile.mesh->vertices) {
                Cartographic c = ellipsoid.cartesianToCartographic(v.positionEcef);
                double h = static_cast<double>(sampleHeightFromDecodedTile(
                    *ownHeightmap, tile.bounds, c.longitude(), c.latitude()));
                Cartographic tc = Cartographic::fromRadians(
                    c.longitude(), c.latitude(), h);
                v.positionEcef = ellipsoid.cartographicToCartesian(tc);
            }
        }
    }

    if (tile.mesh->hasLocalOriginEcef) {
        tile.localOrigin = tile.mesh->localOriginEcef;
    } else {
        tile.localOrigin = Vec3::zero();
    }
    if (!tile.mesh->hasLocalOriginEcef && !tile.mesh->vertices.empty()) {
        for (const auto& v : tile.mesh->vertices) {
            tile.localOrigin += v.positionEcef;
        }
        tile.localOrigin = tile.localOrigin / static_cast<double>(tile.mesh->vertices.size());
    }

    if (tile.mesh->hasHeightRange) {
        setTerrainHeightRange(
            tile,
            tile.mesh->minimumHeight,
            tile.mesh->maximumHeight);
    } else if (ownHeightmap && ownHeightmap->valid()) {
        setTerrainHeightRange(
            tile,
            ownHeightmap->minHeight,
            ownHeightmap->maxHeight);
    } else {
        setDefaultTerrainHeightRange(tile);
    }
    for (TilesetTile* child : tile.children) {
        if (child && !child->meshReady) {
            inheritTerrainHeightRange(*child, tile);
        }
    }

    if (device_ && !tile.mesh->vertices.empty()) {
        std::vector<SurfaceGpuVertex> generatedGpuVertices;
        const std::vector<SurfaceGpuVertex>* gpuVertices =
            tile.mesh->gpuVertices.size() == tile.mesh->vertices.size()
                ? &tile.mesh->gpuVertices
                : nullptr;
        if (!gpuVertices) {
            generatedGpuVertices.resize(tile.mesh->vertices.size());
            for (size_t i = 0; i < tile.mesh->vertices.size(); ++i) {
                const auto& src = tile.mesh->vertices[i];
                SurfaceGpuVertex& dst = generatedGpuVertices[i];
                Vec3 rel = src.positionEcef - tile.localOrigin;
                dst.pos[0] = static_cast<float>(rel.x());
                dst.pos[1] = static_cast<float>(rel.y());
                dst.pos[2] = static_cast<float>(rel.z());
                Vec3 nrm = src.normalEcef;
                if (nrm.lengthSquared() > 0.0) nrm = nrm.normalized();
                else nrm = Ellipsoid::WGS84().geodeticSurfaceNormal(src.positionEcef);
                dst.nrm[0] = static_cast<float>(nrm.x());
                dst.nrm[1] = static_cast<float>(nrm.y());
                dst.nrm[2] = static_cast<float>(nrm.z());
                dst.uv[0] = src.uv[0];
                dst.uv[1] = src.uv[1];
            }
            gpuVertices = &generatedGpuVertices;
        }
        BufferDesc vbDesc;
        vbDesc.size = gpuVertices->size() * sizeof(SurfaceGpuVertex);
        vbDesc.data = gpuVertices->data();
        vbDesc.usage = BufferDesc::Usage::Static;
        vbDesc.type = BufferDesc::Type::Vertex;
        tile.gpuVertexBuffer = device_->createBuffer(vbDesc);

        if (!tile.mesh->indices.empty()) {
            BufferDesc ibDesc;
            ibDesc.size = tile.mesh->indices.size() * sizeof(uint32_t);
            ibDesc.data = tile.mesh->indices.data();
            ibDesc.usage = BufferDesc::Usage::Static;
            ibDesc.type = BufferDesc::Type::Index;
            tile.gpuIndexBuffer = device_->createBuffer(ibDesc);
        }
    }

    tile.meshReady = true;
    tile.surfaceSource = meshSource == SurfaceDrawableSource::None
        ? SurfaceDrawableSource::EllipsoidFallback
        : meshSource;
    tile.contentKind = TileContentKind::Render;
    tile.surfaceDrawable = hasSurfaceDrawable(tile);
    if (hasOwnTerrain || tile.upsampledFromParent || !terrainProvider_) {
        tile.loadState = TileLoadState::Done;
    }
    tile.completeRenderable = isTileCompleteRenderable(tile);
    tile.renderable = tile.completeRenderable;
    markTileResourcesDirty();
    // Bytes recomputed each frame via updateTotalBytesUsed() before unload.
    // No need to incrementally track here — overlay textures may attach/detach
    // later, and recompute captures the current state.
}

namespace {

struct GltfGpuVertex {
    float pos[3];
    float nrm[3];
    float texcoord01[4];
    float color[4];
    float tangent[4];
    float texcoord23[4];
    float texcoord45[4];
    float texcoord67[4];
};

static_assert(
    sizeof(GltfGpuVertex) == 120,
    "glTF GPU vertices pack POSITION, NORMAL, eight TEXCOORD sets, COLOR_0 and TANGENT");

struct GltfGpuInstance {
    float model[16];
    float normal[9];
};

Vec3 transformGltfPoint(const glm::dmat4& transform, const Vec3& point) {
    const glm::dvec4 v = transform * glm::dvec4(point.raw(), 1.0);
    return Vec3(glm::dvec3(v) / v.w);
}

Vec3 gltfPrimitiveCentroid(const GltfPrimitive& primitive) {
    Vec3 centroid = Vec3::zero();
    for (const SurfaceVertex& vertex : primitive.vertices) {
        centroid += vertex.positionEcef;
    }
    return primitive.vertices.empty()
        ? Vec3::zero()
        : centroid / static_cast<double>(primitive.vertices.size());
}

Vec3 computeGltfPrimitiveSortCenterEcef(const GltfPrimitive& primitive,
                                        const Mat4& contentTransform) {
    if (primitive.vertices.empty()) {
        return Vec3::zero();
    }
    const Vec3 centroid = gltfPrimitiveCentroid(primitive);
    if (primitive.instances.empty()) {
        return transformGltfPoint(contentTransform.raw(), centroid);
    }

    Vec3 center = Vec3::zero();
    for (const GltfInstance& instance : primitive.instances) {
        const glm::dmat4 worldTransform =
            contentTransform.raw() * instance.transform.raw();
        center += transformGltfPoint(worldTransform, centroid);
    }
    return center / static_cast<double>(primitive.instances.size());
}

bool gltfPrimitiveUsesSplitBlendInstances(const GltfPrimitive& primitive) {
    return !primitive.instances.empty() &&
           (primitive.alphaMode == GltfAlphaMode::Blend ||
            primitive.transmissionFactor > 0.0f);
}

size_t gltfPrimitiveRenderResourceCount(const GltfPrimitive& primitive) {
    if (primitive.vertices.empty() || primitive.indices.empty()) {
        return 0u;
    }
    return gltfPrimitiveUsesSplitBlendInstances(primitive)
        ? primitive.instances.size()
        : 1u;
}

bool gltfModelUsesSplitBlendInstances(const GltfModel& model) {
    return std::any_of(
        model.primitives.begin(),
        model.primitives.end(),
        gltfPrimitiveUsesSplitBlendInstances);
}

Vec3 computeGltfLocalOrigin(const GltfModel& model,
                            const Mat4& contentTransform) {
    Vec3 origin = Vec3::zero();
    size_t vertexTotal = 0;
    for (const GltfPrimitive& primitive : model.primitives) {
        if (primitive.instances.empty()) {
            for (const SurfaceVertex& vertex : primitive.vertices) {
                origin += contentTransform * vertex.positionEcef;
                ++vertexTotal;
            }
        } else if (!primitive.vertices.empty()) {
            const Vec3 centroid = gltfPrimitiveCentroid(primitive);
            for (const GltfInstance& instance : primitive.instances) {
                const glm::dmat4 worldTransform =
                    contentTransform.raw() * instance.transform.raw();
                origin += transformGltfPoint(worldTransform, centroid) *
                          static_cast<double>(primitive.vertices.size());
                vertexTotal += primitive.vertices.size();
            }
        }
    }
    return vertexTotal > 0
        ? origin / static_cast<double>(vertexTotal)
        : Vec3::zero();
}

bool gltfPrimitiveHasTexCoordSet(
    const GltfPrimitive& primitive,
    int texCoordSet) {
    if (texCoordSet < 0 ||
        texCoordSet >= static_cast<int>(kGltfMaxTexCoordSets)) {
        return false;
    }
    const size_t set = static_cast<size_t>(texCoordSet);
    if (primitive.vertexTexCoords[set].size() == primitive.vertices.size()) {
        return true;
    }
    return set == 0 && !primitive.vertices.empty();
}

std::array<float, 2> gltfTexCoordForVertex(
    const GltfPrimitive& primitive,
    size_t texCoordSet,
    size_t vertexIndex) {
    if (texCoordSet < kGltfMaxTexCoordSets &&
        primitive.vertexTexCoords[texCoordSet].size() ==
            primitive.vertices.size()) {
        return primitive.vertexTexCoords[texCoordSet][vertexIndex];
    }
    if (texCoordSet == 0 && vertexIndex < primitive.vertices.size()) {
        return primitive.vertices[vertexIndex].uv;
    }
    return {0.0f, 0.0f};
}

void packTexCoordPair(
    float out[4],
    const std::array<float, 2>& first,
    const std::array<float, 2>& second) {
    out[0] = first[0];
    out[1] = first[1];
    out[2] = second[0];
    out[3] = second[1];
}

std::vector<GltfGpuVertex> buildGltfGpuVertices(
    const GltfPrimitive& primitive,
    const Mat4& contentTransform,
    const Vec3& localOrigin,
    std::optional<bool> keepInstanceLocalVertices = std::nullopt) {
    std::vector<GltfGpuVertex> verts(primitive.vertices.size());
    const bool instanced = keepInstanceLocalVertices.value_or(
        !primitive.instances.empty());
    const bool hasVertexColors =
        primitive.vertexColors.size() == primitive.vertices.size();
    const bool hasTangents =
        primitive.vertexTangents.size() == primitive.vertices.size();
    const glm::dmat3 tangentMatrix(contentTransform.raw());
    const glm::dmat3 normalMatrix = glm::inverseTranspose(
        glm::dmat3(contentTransform.raw()));
    for (size_t i = 0; i < primitive.vertices.size(); ++i) {
        const SurfaceVertex& src = primitive.vertices[i];
        const Vec3 rel = instanced
            ? src.positionEcef
            : (contentTransform * src.positionEcef) - localOrigin;
        verts[i].pos[0] = static_cast<float>(rel.x());
        verts[i].pos[1] = static_cast<float>(rel.y());
        verts[i].pos[2] = static_cast<float>(rel.z());

        Vec3 nrm = instanced
            ? src.normalEcef
            : Vec3(normalMatrix * src.normalEcef.raw());
        if (nrm.lengthSquared() > 0.0) {
            nrm = nrm.normalized();
        } else {
            nrm = Vec3::unitZ();
        }
        verts[i].nrm[0] = static_cast<float>(nrm.x());
        verts[i].nrm[1] = static_cast<float>(nrm.y());
        verts[i].nrm[2] = static_cast<float>(nrm.z());
        packTexCoordPair(
            verts[i].texcoord01,
            gltfTexCoordForVertex(primitive, 0, i),
            gltfTexCoordForVertex(primitive, 1, i));
        const std::array<float, 4> color = hasVertexColors
            ? primitive.vertexColors[i]
            : std::array<float, 4>{1.0f, 1.0f, 1.0f, 1.0f};
        verts[i].color[0] = color[0];
        verts[i].color[1] = color[1];
        verts[i].color[2] = color[2];
        verts[i].color[3] = color[3];
        const std::array<float, 4> tangent = hasTangents
            ? primitive.vertexTangents[i]
            : std::array<float, 4>{0.0f, 0.0f, 0.0f, 1.0f};
        glm::dvec3 tangentDirection(tangent[0], tangent[1], tangent[2]);
        if (hasTangents && !instanced) {
            tangentDirection = tangentMatrix * tangentDirection;
            const double lenSq = glm::dot(tangentDirection, tangentDirection);
            if (std::isfinite(lenSq) && lenSq > 0.0) {
                tangentDirection = glm::normalize(tangentDirection);
            } else {
                tangentDirection = glm::dvec3(0.0);
            }
        }
        verts[i].tangent[0] = static_cast<float>(tangentDirection.x);
        verts[i].tangent[1] = static_cast<float>(tangentDirection.y);
        verts[i].tangent[2] = static_cast<float>(tangentDirection.z);
        verts[i].tangent[3] = tangent[3];
        packTexCoordPair(
            verts[i].texcoord23,
            gltfTexCoordForVertex(primitive, 2, i),
            gltfTexCoordForVertex(primitive, 3, i));
        packTexCoordPair(
            verts[i].texcoord45,
            gltfTexCoordForVertex(primitive, 4, i),
            gltfTexCoordForVertex(primitive, 5, i));
        packTexCoordPair(
            verts[i].texcoord67,
            gltfTexCoordForVertex(primitive, 6, i),
            gltfTexCoordForVertex(primitive, 7, i));
    }
    return verts;
}

} // namespace

void Tileset::ensureGltfRenderResources(TilesetTile& tile) {
    if (!tile.gltfModel) return;
    const bool animated = tile.gltfModel->hasRuntimeAnimation();
    const bool animationChanged =
        animated && tile.gltfModel->updateAnimation(currentFrameTimeSeconds_);
    size_t expectedResourceCount = 0;
    for (const GltfPrimitive& primitive : tile.gltfModel->primitives) {
        expectedResourceCount += gltfPrimitiveRenderResourceCount(primitive);
    }
    const bool splitBlendInstances =
        gltfModelUsesSplitBlendInstances(*tile.gltfModel);
    if (expectedResourceCount > 0 &&
        tile.gltfPrimitiveResources.size() == expectedResourceCount) {
        const bool allReady = std::all_of(
            tile.gltfPrimitiveResources.begin(),
            tile.gltfPrimitiveResources.end(),
            [](const GltfPrimitiveRenderResources& resources) {
                return resources.vertexBuffer != nullptr &&
                       resources.indexBuffer != nullptr &&
                       resources.indexCount > 0 &&
                       (resources.instanceCount <= 0 ||
                        resources.instanceBuffer != nullptr);
            });
        if (allReady) {
            if (animated && animationChanged && splitBlendInstances) {
                tile.gltfPrimitiveResources.clear();
            } else if (animated && animationChanged && device_) {
                tile.localOrigin = computeGltfLocalOrigin(
                    *tile.gltfModel,
                    tile.gltfContentTransform);
                bool updated = true;
                size_t resourceIndex = 0;
                for (const GltfPrimitive& primitive :
                     tile.gltfModel->primitives) {
                    if (primitive.vertices.empty() ||
                        primitive.indices.empty()) {
                        continue;
                    }
                    if (resourceIndex >= tile.gltfPrimitiveResources.size()) {
                        updated = false;
                        break;
                    }
                    GltfPrimitiveRenderResources& resources =
                        tile.gltfPrimitiveResources[resourceIndex++];
                    std::vector<GltfGpuVertex> verts =
                        buildGltfGpuVertices(
                            primitive,
                            tile.gltfContentTransform,
                            tile.localOrigin);
                    const bool ok = resources.vertexBuffer &&
                        resources.vertexBuffer->size() ==
                            verts.size() * sizeof(GltfGpuVertex) &&
                        device_->updateBuffer(
                            resources.vertexBuffer.get(),
                            0,
                            verts.data(),
                            verts.size() * sizeof(GltfGpuVertex));
                    if (!ok) {
                        updated = false;
                        break;
                    }
                    resources.sortCenterEcef =
                        computeGltfPrimitiveSortCenterEcef(
                            primitive,
                            tile.gltfContentTransform);
                    resources.animationRevision =
                        tile.gltfModel->currentAnimationRevision();
                }
                if (!updated) {
                    tile.gltfPrimitiveResources.clear();
                } else {
                    tile.meshReady = true;
                    tile.loadState = TileLoadState::Done;
                    tile.contentKind = TileContentKind::Render;
                    return;
                }
            } else {
                tile.meshReady = true;
                tile.loadState = TileLoadState::Done;
                tile.contentKind = TileContentKind::Render;
                return;
            }
        }
    }
    if (!device_) return;
    tile.localOrigin = computeGltfLocalOrigin(
        *tile.gltfModel,
        tile.gltfContentTransform);

    tile.gltfTextureResources.clear();
    tile.gltfTextureResources.reserve(tile.gltfModel->textures.size());
    for (const GltfTexture& texture : tile.gltfModel->textures) {
        tile.gltfTextureResources.push_back(
            createGltfGpuTexture(device_, texture));
    }

    tile.gltfPrimitiveResources.clear();
    tile.gltfPrimitiveResources.reserve(expectedResourceCount);
    bool resourceFailure = false;

    for (const GltfPrimitive& primitive : tile.gltfModel->primitives) {
        if (primitive.vertices.empty() || primitive.indices.empty()) {
            continue;
        }

        const bool instanced = !primitive.instances.empty();

        auto appendPrimitiveResource =
            [&](std::vector<GltfGpuVertex>&& verts,
                const Vec3& sortCenter,
                const std::vector<GltfGpuInstance>* instanceData) {
                GltfPrimitiveRenderResources resources;
                BufferDesc vbDesc;
                vbDesc.size = verts.size() * sizeof(GltfGpuVertex);
                vbDesc.data = verts.data();
                vbDesc.usage = animated
                    ? BufferDesc::Usage::Dynamic
                    : BufferDesc::Usage::Static;
                vbDesc.type = BufferDesc::Type::Vertex;
                resources.vertexBuffer = device_->createBuffer(vbDesc);

                BufferDesc ibDesc;
                ibDesc.size = primitive.indices.size() * sizeof(uint32_t);
                ibDesc.data = primitive.indices.data();
                ibDesc.usage = BufferDesc::Usage::Static;
                ibDesc.type = BufferDesc::Type::Index;
                resources.indexBuffer = device_->createBuffer(ibDesc);
                resources.vertexCount =
                    static_cast<int>(primitive.vertices.size());
                resources.indexCount =
                    static_cast<int>(primitive.indices.size());
                resources.primitiveMode = primitive.primitiveMode;
                resources.sortCenterEcef = sortCenter;
                resources.baseColorFactor = primitive.baseColorFactor;
                resources.metallicFactor = primitive.metallicFactor;
                resources.roughnessFactor = primitive.roughnessFactor;
                resources.dielectricSpecularF0 =
                    primitive.dielectricSpecularF0;
                resources.specularFactor = primitive.specularFactor;
                resources.specularColorFactor =
                    primitive.specularColorFactor;
                resources.specularGlossinessWorkflow =
                    primitive.specularGlossinessWorkflow;
                resources.specularGlossinessSpecularFactor =
                    primitive.specularGlossinessSpecularFactor;
                resources.specularGlossinessGlossinessFactor =
                    primitive.specularGlossinessGlossinessFactor;
                resources.transmissionFactor = primitive.transmissionFactor;
                resources.anisotropyStrength =
                    primitive.anisotropyStrength;
                resources.anisotropyRotation =
                    primitive.anisotropyRotation;
                resources.clearcoatFactor = primitive.clearcoatFactor;
                resources.clearcoatRoughnessFactor =
                    primitive.clearcoatRoughnessFactor;
                resources.clearcoatNormalTextureScale =
                    primitive.clearcoatNormalTextureScale;
                resources.sheenColorFactor = primitive.sheenColorFactor;
                resources.sheenRoughnessFactor =
                    primitive.sheenRoughnessFactor;
                resources.normalTextureScale = primitive.normalTextureScale;
                resources.occlusionTextureStrength =
                    primitive.occlusionTextureStrength;
                resources.emissiveFactor = primitive.emissiveFactor;
                resources.alphaMode = primitive.alphaMode;
                resources.alphaCutoff = primitive.alphaCutoff;
                resources.doubleSided = primitive.doubleSided;
                resources.unlit = primitive.unlit;
                resources.dynamicVertices = animated;
                resources.animationRevision =
                    tile.gltfModel->currentAnimationRevision();
                std::optional<GltfTextureBinding> baseColorBinding =
                    primitive.baseColorTexture;
                if (!baseColorBinding && primitive.baseColorTextureIndex) {
                    GltfTextureBinding binding;
                    binding.textureIndex = *primitive.baseColorTextureIndex;
                    baseColorBinding = binding;
                }
                resources.baseColorTexture = makeGltfTextureBinding(
                    baseColorBinding,
                    tile.gltfTextureResources);
                resources.metallicRoughnessTexture = makeGltfTextureBinding(
                    primitive.metallicRoughnessTexture,
                    tile.gltfTextureResources);
                resources.anisotropyTexture = makeGltfTextureBinding(
                    primitive.anisotropyTexture,
                    tile.gltfTextureResources);
                resources.specularTexture = makeGltfTextureBinding(
                    primitive.specularTexture,
                    tile.gltfTextureResources);
                resources.specularColorTexture = makeGltfTextureBinding(
                    primitive.specularColorTexture,
                    tile.gltfTextureResources);
                resources.specularGlossinessTexture = makeGltfTextureBinding(
                    primitive.specularGlossinessTexture,
                    tile.gltfTextureResources);
                resources.transmissionTexture = makeGltfTextureBinding(
                    primitive.transmissionTexture,
                    tile.gltfTextureResources);
                resources.clearcoatTexture = makeGltfTextureBinding(
                    primitive.clearcoatTexture,
                    tile.gltfTextureResources);
                resources.clearcoatRoughnessTexture = makeGltfTextureBinding(
                    primitive.clearcoatRoughnessTexture,
                    tile.gltfTextureResources);
                resources.clearcoatNormalTexture = makeGltfTextureBinding(
                    primitive.clearcoatNormalTexture,
                    tile.gltfTextureResources);
                resources.sheenColorTexture = makeGltfTextureBinding(
                    primitive.sheenColorTexture,
                    tile.gltfTextureResources);
                resources.sheenRoughnessTexture = makeGltfTextureBinding(
                    primitive.sheenRoughnessTexture,
                    tile.gltfTextureResources);
                resources.normalTexture = makeGltfTextureBinding(
                    primitive.normalTexture,
                    tile.gltfTextureResources);
                resources.occlusionTexture = makeGltfTextureBinding(
                    primitive.occlusionTexture,
                    tile.gltfTextureResources);
                resources.emissiveTexture = makeGltfTextureBinding(
                    primitive.emissiveTexture,
                    tile.gltfTextureResources);
                auto bindingHasTexCoordSet =
                    [&](const std::optional<GltfTextureBinding>& binding) {
                        return !binding ||
                               gltfPrimitiveHasTexCoordSet(
                                   primitive,
                                   binding->texCoord);
                    };
                if ((baseColorBinding &&
                     !resources.baseColorTexture.texture) ||
                    (primitive.metallicRoughnessTexture &&
                     !resources.metallicRoughnessTexture.texture) ||
                    (primitive.anisotropyTexture &&
                     !resources.anisotropyTexture.texture) ||
                    (primitive.specularTexture &&
                     !resources.specularTexture.texture) ||
                    (primitive.specularColorTexture &&
                     !resources.specularColorTexture.texture) ||
                    (primitive.specularGlossinessTexture &&
                     !resources.specularGlossinessTexture.texture) ||
                    (primitive.transmissionTexture &&
                     !resources.transmissionTexture.texture) ||
                    (primitive.clearcoatTexture &&
                     !resources.clearcoatTexture.texture) ||
                    (primitive.clearcoatRoughnessTexture &&
                     !resources.clearcoatRoughnessTexture.texture) ||
                    (primitive.clearcoatNormalTexture &&
                     !resources.clearcoatNormalTexture.texture) ||
                    (primitive.sheenColorTexture &&
                     !resources.sheenColorTexture.texture) ||
                    (primitive.sheenRoughnessTexture &&
                     !resources.sheenRoughnessTexture.texture) ||
                    (primitive.normalTexture &&
                     !resources.normalTexture.texture) ||
                    (primitive.occlusionTexture &&
                     !resources.occlusionTexture.texture) ||
                    (primitive.emissiveTexture &&
                     !resources.emissiveTexture.texture) ||
                    !bindingHasTexCoordSet(baseColorBinding) ||
                    !bindingHasTexCoordSet(
                        primitive.metallicRoughnessTexture) ||
                    !bindingHasTexCoordSet(primitive.anisotropyTexture) ||
                    !bindingHasTexCoordSet(primitive.specularTexture) ||
                    !bindingHasTexCoordSet(
                        primitive.specularColorTexture) ||
                    !bindingHasTexCoordSet(
                        primitive.specularGlossinessTexture) ||
                    !bindingHasTexCoordSet(primitive.transmissionTexture) ||
                    !bindingHasTexCoordSet(primitive.clearcoatTexture) ||
                    !bindingHasTexCoordSet(
                        primitive.clearcoatRoughnessTexture) ||
                    !bindingHasTexCoordSet(
                        primitive.clearcoatNormalTexture) ||
                    !bindingHasTexCoordSet(primitive.sheenColorTexture) ||
                    !bindingHasTexCoordSet(
                        primitive.sheenRoughnessTexture) ||
                    !bindingHasTexCoordSet(primitive.normalTexture) ||
                    !bindingHasTexCoordSet(primitive.occlusionTexture) ||
                    !bindingHasTexCoordSet(primitive.emissiveTexture)) {
                    return false;
                }

                if (instanceData) {
                    BufferDesc instanceDesc;
                    instanceDesc.size =
                        instanceData->size() * sizeof(GltfGpuInstance);
                    instanceDesc.data = instanceData->data();
                    instanceDesc.usage = BufferDesc::Usage::Static;
                    instanceDesc.type = BufferDesc::Type::Vertex;
                    resources.instanceBuffer =
                        device_->createBuffer(instanceDesc);
                    resources.instanceCount =
                        static_cast<int>(instanceData->size());
                }

                if (!resources.vertexBuffer ||
                    !resources.indexBuffer ||
                    (instanceData && !resources.instanceBuffer)) {
                    return false;
                }

                tile.gltfPrimitiveResources.push_back(std::move(resources));
                return true;
            };

        if (gltfPrimitiveUsesSplitBlendInstances(primitive)) {
            const Vec3 centroid = gltfPrimitiveCentroid(primitive);
            for (const GltfInstance& instance : primitive.instances) {
                const Mat4 instanceTransform(
                    tile.gltfContentTransform.raw() *
                    instance.transform.raw());
                std::vector<GltfGpuVertex> verts =
                    buildGltfGpuVertices(
                        primitive,
                        instanceTransform,
                        tile.localOrigin,
                        false);
                const Vec3 sortCenter =
                    transformGltfPoint(instanceTransform.raw(), centroid);
                if (!appendPrimitiveResource(
                        std::move(verts),
                        sortCenter,
                        nullptr)) {
                    resourceFailure = true;
                    break;
                }
            }
            if (resourceFailure) {
                break;
            }
            continue;
        }

        std::vector<GltfGpuVertex> verts =
            buildGltfGpuVertices(
                primitive,
                tile.gltfContentTransform,
                tile.localOrigin);

        std::vector<GltfGpuInstance> instances;
        if (instanced) {
            instances.resize(primitive.instances.size());
            for (size_t i = 0; i < primitive.instances.size(); ++i) {
                glm::dmat4 model =
                    tile.gltfContentTransform.raw() *
                    primitive.instances[i].transform.raw();
                model[3].x -= tile.localOrigin.x();
                model[3].y -= tile.localOrigin.y();
                model[3].z -= tile.localOrigin.z();

                glm::dmat3 normal(1.0);
                const glm::dmat3 model3(model);
                const double det = glm::determinant(model3);
                if (std::isfinite(det) && std::abs(det) > 1e-14) {
                    normal = glm::inverseTranspose(model3);
                }

                for (int c = 0; c < 4; ++c) {
                    for (int r = 0; r < 4; ++r) {
                        instances[i].model[c * 4 + r] =
                            static_cast<float>(model[c][r]);
                    }
                }
                for (int c = 0; c < 3; ++c) {
                    for (int r = 0; r < 3; ++r) {
                        instances[i].normal[c * 3 + r] =
                            static_cast<float>(normal[c][r]);
                    }
                }
            }
        }

        const std::vector<GltfGpuInstance>* instanceData =
            instanced ? &instances : nullptr;
        if (!appendPrimitiveResource(
                std::move(verts),
                computeGltfPrimitiveSortCenterEcef(
                    primitive,
                    tile.gltfContentTransform),
                instanceData)) {
            resourceFailure = true;
            break;
        }
    }

    if (resourceFailure) {
        tile.gltfPrimitiveResources.clear();
        tile.gltfTextureResources.clear();
    }

    tile.meshReady =
        !resourceFailure && !tile.gltfPrimitiveResources.empty();
    tile.loadState = tile.meshReady
        ? TileLoadState::Done
        : TileLoadState::FailedTemporarily;
    tile.contentKind = tile.meshReady
        ? TileContentKind::Render
        : TileContentKind::Unknown;
}

void Tileset::buildGltfDrawCommands(Renderer& renderer,
                                    TilesetTile& tile,
                                    RenderCommandList& commands,
                                    float transitionOpacity) {
    ensureGltfRenderResources(tile);
    if (!tile.meshReady || tile.gltfPrimitiveResources.empty()) {
        return;
    }

    for (GltfPrimitiveRenderResources& primitive :
         tile.gltfPrimitiveResources) {
        if (!primitive.vertexBuffer || !primitive.indexBuffer ||
            primitive.indexCount <= 0) {
            continue;
        }

        RenderCommand cmd = primitive.instanceCount > 0
            ? renderer.makeGltfPrimitiveInstancedCommand(
                  primitive.vertexBuffer.get(),
                  primitive.indexBuffer.get(),
                  primitive.instanceBuffer.get(),
                  primitive.indexCount,
                  primitive.vertexCount,
                  primitive.instanceCount)
            : renderer.makeGltfPrimitiveCommand(
                  primitive.vertexBuffer.get(),
                  primitive.indexBuffer.get(),
                  primitive.indexCount,
                  primitive.vertexCount);
        cmd.frameId = frameNumber_;
        cmd.generation = generation_;
        cmd.primitive = renderPrimitiveType(primitive.primitiveMode);
        cmd.uniforms["u_modelOrigin"] = {
            static_cast<float>(tile.localOrigin.x()),
            static_cast<float>(tile.localOrigin.y()),
            static_cast<float>(tile.localOrigin.z())
        };
        cmd.hasWorldSortCenter = true;
        cmd.worldSortCenter = {
            primitive.sortCenterEcef.x(),
            primitive.sortCenterEcef.y(),
            primitive.sortCenterEcef.z()
        };
        cmd.uniforms["u_renderOpacity"] = {transitionOpacity};
        cmd.uniforms["u_baseColor"] = {
            primitive.baseColorFactor[0],
            primitive.baseColorFactor[1],
            primitive.baseColorFactor[2],
            primitive.baseColorFactor[3]
        };
        cmd.uniforms["u_hasBaseColorTexture"] = {
            primitive.baseColorTexture.texture ? 1.0f : 0.0f
        };
        cmd.uniforms["u_materialFactors"] = {
            primitive.metallicFactor,
            primitive.roughnessFactor,
            primitive.normalTextureScale,
            primitive.occlusionTextureStrength
        };
        cmd.uniforms["u_dielectricSpecularF0"] = {
            primitive.dielectricSpecularF0
        };
        cmd.uniforms["u_specularFactor"] = {
            primitive.specularFactor
        };
        cmd.uniforms["u_specularColorFactor"] = {
            primitive.specularColorFactor[0],
            primitive.specularColorFactor[1],
            primitive.specularColorFactor[2]
        };
        cmd.uniforms["u_specularGlossinessWorkflow"] = {
            primitive.specularGlossinessWorkflow ? 1.0f : 0.0f
        };
        cmd.uniforms["u_specularGlossinessFactor"] = {
            primitive.specularGlossinessSpecularFactor[0],
            primitive.specularGlossinessSpecularFactor[1],
            primitive.specularGlossinessSpecularFactor[2],
            primitive.specularGlossinessGlossinessFactor
        };
        cmd.uniforms["u_transmissionFactor"] = {
            primitive.transmissionFactor
        };
        cmd.uniforms["u_anisotropyFactors"] = {
            primitive.anisotropyStrength,
            primitive.anisotropyRotation
        };
        cmd.uniforms["u_clearcoatFactors"] = {
            primitive.clearcoatFactor,
            primitive.clearcoatRoughnessFactor,
            primitive.clearcoatNormalTextureScale
        };
        cmd.uniforms["u_sheenColorFactor"] = {
            primitive.sheenColorFactor[0],
            primitive.sheenColorFactor[1],
            primitive.sheenColorFactor[2]
        };
        cmd.uniforms["u_sheenRoughnessFactor"] = {
            primitive.sheenRoughnessFactor
        };
        cmd.uniforms["u_hasMaterialTextures"] = {
            primitive.metallicRoughnessTexture.texture ? 1.0f : 0.0f,
            primitive.normalTexture.texture ? 1.0f : 0.0f,
            primitive.occlusionTexture.texture ? 1.0f : 0.0f,
            primitive.emissiveTexture.texture ? 1.0f : 0.0f
        };
        cmd.uniforms["u_hasAnisotropyTexture"] = {
            primitive.anisotropyTexture.texture ? 1.0f : 0.0f
        };
        cmd.uniforms["u_hasSpecularTextures"] = {
            primitive.specularTexture.texture ? 1.0f : 0.0f,
            primitive.specularColorTexture.texture ? 1.0f : 0.0f
        };
        cmd.uniforms["u_hasSpecularGlossinessTexture"] = {
            primitive.specularGlossinessTexture.texture ? 1.0f : 0.0f
        };
        cmd.uniforms["u_hasTransmissionTexture"] = {
            primitive.transmissionTexture.texture ? 1.0f : 0.0f
        };
        cmd.uniforms["u_hasClearcoatTextures"] = {
            primitive.clearcoatTexture.texture ? 1.0f : 0.0f,
            primitive.clearcoatRoughnessTexture.texture ? 1.0f : 0.0f,
            primitive.clearcoatNormalTexture.texture ? 1.0f : 0.0f
        };
        cmd.uniforms["u_hasSheenTextures"] = {
            primitive.sheenColorTexture.texture ? 1.0f : 0.0f,
            primitive.sheenRoughnessTexture.texture ? 1.0f : 0.0f
        };
        cmd.uniforms["u_emissiveFactor"] = {
            primitive.emissiveFactor[0],
            primitive.emissiveFactor[1],
            primitive.emissiveFactor[2]
        };
        cmd.uniforms["u_textureCoordSets"] = {
            static_cast<float>(primitive.baseColorTexture.texCoord),
            static_cast<float>(primitive.metallicRoughnessTexture.texCoord),
            static_cast<float>(primitive.normalTexture.texCoord),
            static_cast<float>(primitive.occlusionTexture.texCoord)
        };
        cmd.uniforms["u_emissiveTexCoordSet"] = {
            static_cast<float>(primitive.emissiveTexture.texCoord)
        };
        cmd.uniforms["u_anisotropyTexCoordSet"] = {
            static_cast<float>(primitive.anisotropyTexture.texCoord)
        };
        cmd.uniforms["u_specularTexCoordSets"] = {
            static_cast<float>(primitive.specularTexture.texCoord),
            static_cast<float>(primitive.specularColorTexture.texCoord)
        };
        cmd.uniforms["u_specularGlossinessTexCoordSet"] = {
            static_cast<float>(primitive.specularGlossinessTexture.texCoord)
        };
        cmd.uniforms["u_transmissionTexCoordSet"] = {
            static_cast<float>(primitive.transmissionTexture.texCoord)
        };
        cmd.uniforms["u_clearcoatTexCoordSets"] = {
            static_cast<float>(primitive.clearcoatTexture.texCoord),
            static_cast<float>(primitive.clearcoatRoughnessTexture.texCoord),
            static_cast<float>(primitive.clearcoatNormalTexture.texCoord)
        };
        cmd.uniforms["u_sheenTexCoordSets"] = {
            static_cast<float>(primitive.sheenColorTexture.texCoord),
            static_cast<float>(primitive.sheenRoughnessTexture.texCoord)
        };
        cmd.uniforms["u_alphaMode"] = {
            alphaModeUniform(primitive.alphaMode)
        };
        cmd.uniforms["u_alphaCutoff"] = {
            primitive.alphaCutoff
        };
        cmd.uniforms["u_unlit"] = {
            primitive.unlit ? 1.0f : 0.0f
        };
        auto setTransformUniforms = [&cmd](
            const char* offsetScaleName,
            const char* rotationName,
            const GltfPrimitiveRenderResources::TextureBinding& binding) {
            cmd.uniforms[offsetScaleName] = {
                binding.offsetScale[0],
                binding.offsetScale[1],
                binding.offsetScale[2],
                binding.offsetScale[3]
            };
            cmd.uniforms[rotationName] = {
                binding.rotationSinCos[0],
                binding.rotationSinCos[1]
            };
        };
        setTransformUniforms(
            "u_baseColorTexOffsetScale",
            "u_baseColorTexRotationSinCos",
            primitive.baseColorTexture);
        setTransformUniforms(
            "u_metallicRoughnessTexOffsetScale",
            "u_metallicRoughnessTexRotationSinCos",
            primitive.metallicRoughnessTexture);
        setTransformUniforms(
            "u_anisotropyTexOffsetScale",
            "u_anisotropyTexRotationSinCos",
            primitive.anisotropyTexture);
        setTransformUniforms(
            "u_specularTexOffsetScale",
            "u_specularTexRotationSinCos",
            primitive.specularTexture);
        setTransformUniforms(
            "u_specularColorTexOffsetScale",
            "u_specularColorTexRotationSinCos",
            primitive.specularColorTexture);
        setTransformUniforms(
            "u_specularGlossinessTexOffsetScale",
            "u_specularGlossinessTexRotationSinCos",
            primitive.specularGlossinessTexture);
        setTransformUniforms(
            "u_transmissionTexOffsetScale",
            "u_transmissionTexRotationSinCos",
            primitive.transmissionTexture);
        setTransformUniforms(
            "u_clearcoatTexOffsetScale",
            "u_clearcoatTexRotationSinCos",
            primitive.clearcoatTexture);
        setTransformUniforms(
            "u_clearcoatRoughnessTexOffsetScale",
            "u_clearcoatRoughnessTexRotationSinCos",
            primitive.clearcoatRoughnessTexture);
        setTransformUniforms(
            "u_clearcoatNormalTexOffsetScale",
            "u_clearcoatNormalTexRotationSinCos",
            primitive.clearcoatNormalTexture);
        setTransformUniforms(
            "u_sheenColorTexOffsetScale",
            "u_sheenColorTexRotationSinCos",
            primitive.sheenColorTexture);
        setTransformUniforms(
            "u_sheenRoughnessTexOffsetScale",
            "u_sheenRoughnessTexRotationSinCos",
            primitive.sheenRoughnessTexture);
        setTransformUniforms(
            "u_normalTexOffsetScale",
            "u_normalTexRotationSinCos",
            primitive.normalTexture);
        setTransformUniforms(
            "u_occlusionTexOffsetScale",
            "u_occlusionTexRotationSinCos",
            primitive.occlusionTexture);
        setTransformUniforms(
            "u_emissiveTexOffsetScale",
            "u_emissiveTexRotationSinCos",
            primitive.emissiveTexture);

        cmd.textures.resize(15, nullptr);
        cmd.textures[0] = primitive.baseColorTexture.texture;
        cmd.textures[1] = primitive.metallicRoughnessTexture.texture;
        cmd.textures[2] = primitive.normalTexture.texture;
        cmd.textures[3] = primitive.occlusionTexture.texture;
        cmd.textures[4] = primitive.emissiveTexture.texture;
        cmd.textures[5] = primitive.specularTexture.texture;
        cmd.textures[6] = primitive.specularColorTexture.texture;
        cmd.textures[7] = primitive.clearcoatTexture.texture;
        cmd.textures[8] = primitive.clearcoatRoughnessTexture.texture;
        cmd.textures[9] = primitive.clearcoatNormalTexture.texture;
        cmd.textures[10] = primitive.sheenColorTexture.texture;
        cmd.textures[11] = primitive.sheenRoughnessTexture.texture;
        cmd.textures[12] = primitive.anisotropyTexture.texture;
        cmd.textures[13] = primitive.specularGlossinessTexture.texture;
        cmd.textures[14] = primitive.transmissionTexture.texture;
        cmd.cullFace = !primitive.doubleSided;
        if (transitionOpacity < 0.999f ||
            primitive.alphaMode == GltfAlphaMode::Blend ||
            primitive.transmissionFactor > 0.0f) {
            cmd.blend = true;
            cmd.depthWrite = false;
            cmd.blendSrc = RenderCommand::BlendFactor::SrcAlpha;
            cmd.blendDst = RenderCommand::BlendFactorDst::OneMinusSrcAlpha;
        }
        commands.push_back(std::move(cmd));
    }
}

void Tileset::buildTileDrawCommand(
    Renderer& renderer,
    TilesetTile& tile,
    RenderCommandList& commands,
    float transitionOpacity,
    bool allowSynchronousMeshPrep,
    const std::optional<std::array<float, 4>>& surfaceClipUv) {
    if (tile.gltfModel) {
        buildGltfDrawCommands(renderer, tile, commands, transitionOpacity);
        return;
    }

    if (!tile.meshReady) {
        if (!allowSynchronousMeshPrep) {
            return;
        }
        ensureTileMesh(tile);
    }
    if (!hasSurfaceDrawable(tile)) return;

    if (tile.rasterOverlays.size() < rasterOverlays_.size()) {
        tile.rasterOverlays.resize(rasterOverlays_.size());
    }
    tile.missingRasterOverlayProjections.clear();
    const RasterOverlayDetails& overlayDetails =
        tile.mesh ? tile.mesh->rasterOverlayDetails : RasterOverlayDetails{};

    // ── cesium-native: drive 7-step state machine for each overlay ──
    std::optional<size_t> firstMoreDetailAvailable;
    std::optional<size_t> firstUnknownAvailability;
    const std::vector<size_t> overlayOrder = rasterOverlayProcessingOrder();
    for (size_t i : overlayOrder) {
        if (i >= tile.rasterOverlays.size()) {
            continue;
        }
        auto* activeOverlay = rasterOverlays_[i];
        if (!activeOverlay || !activeOverlay->visible()) {
            continue;
        }
        RasterOverlayTileProvider* activeProvider =
            activeOverlay->ensureTileProvider(device_);
        if (!activeProvider) continue;
        auto& overlay = tile.rasterOverlays[i];
        if (!overlay) {
            overlay = std::make_unique<RasterMappedToTilesetTile>();
        }
        const RasterOverlayProjection projection =
            activeProvider->getProjection();
        const Rectangle* geometryRectangle =
            overlayDetails.findRectangleForOverlayProjection(projection);
        std::optional<Rectangle> boundingRegionRectangle;
        if (!geometryRectangle &&
            tile.boundingVolume &&
            tile.boundingVolume->kind == TileBoundingVolumeKind::Region) {
            boundingRegionRectangle = tile.boundingVolume->region;
        }
        const Rectangle& rasterTargetRectangle = geometryRectangle
            ? *geometryRectangle
            : (boundingRegionRectangle ? *boundingRegionRectangle : tile.bounds);
        const RasterTargetScreenPixels rasterScreenPixels =
            computeDesiredRasterScreenPixels(
                rasterTargetRectangle,
                tile.geometricError,
                options_.maximumScreenSpaceError);
        const RasterMappedToTilesetTile::MoreDetail moreDetail =
            overlay->update(
                tile.key,
                overlayDetails,
                rasterScreenPixels.x,
                rasterScreenPixels.y,
                *activeProvider,
                &renderer,
                tile.missingRasterOverlayProjections,
                tile.parent,
                i,
                tile.boundingVolume ? &*tile.boundingVolume : nullptr,
                tile.mesh != nullptr);
        if (!tile.missingRasterOverlayProjections.empty()) {
            unloadTileContent(tile, &renderer);
            return;
        }
        if (moreDetail == RasterMappedToTilesetTile::MoreDetail::Yes &&
            !firstMoreDetailAvailable) {
            firstMoreDetailAvailable = i;
        } else if (
            moreDetail == RasterMappedToTilesetTile::MoreDetail::Unknown &&
            !firstUnknownAvailability) {
            firstUnknownAvailability = i;
        }
        overlay->loadThrottled(*activeProvider, &frameResourceBudget_);
    }

    tile.surfaceDrawable = hasSurfaceDrawable(tile);
    tile.completeRenderable = isTileCompleteRenderable(tile);
    tile.renderable = tile.completeRenderable;

    const bool shouldCreateRasterUpsampledChildren =
        firstMoreDetailAvailable &&
        (!firstUnknownAvailability ||
         *firstUnknownAvailability > *firstMoreDetailAvailable);
    if (shouldCreateRasterUpsampledChildren && tile.children.empty()) {
        createRasterOverlayUpsampledChildren(tile);
    }

    Texture* baseTexture = nullptr;
    SurfaceRasterBinding baseBinding;
    std::optional<size_t> baseOverlayIndex;

    for (size_t i : overlayOrder) {
        if (i >= tile.rasterOverlays.size()) {
            continue;
        }
        auto* activeOverlay = rasterOverlays_[i];
        if (!activeOverlay || !activeOverlay->visible() ||
            activeOverlay->getOverlay().role() != RasterOverlayRole::BaseImagery) {
            continue;
        }

        const RasterMappedToTilesetTile* mapped =
            tile.rasterOverlays[i].get();
        const SurfaceRasterBinding binding =
            chooseSurfaceRasterBinding(mapped);
        if (overlayBindingAllowedByPolicy(activeOverlay, mapped, binding)) {
            baseTexture = binding.tile->getTexture();
            baseBinding = binding;
            baseOverlayIndex = i;
            break;
        }
    }

    if (!baseTexture) {
        return;
    }

    const int meshIndexCount = static_cast<int>(tile.mesh->indices.size());
    int surfaceIndexOffset = 0;
    int surfaceIndexCount = meshIndexCount;
    const SkirtMetadata& skirt = tile.mesh->skirtMeta;
    if (skirt.noSkirtIndicesCount > 0 &&
        skirt.noSkirtIndicesBegin < tile.mesh->indices.size() &&
        skirt.noSkirtIndicesBegin + skirt.noSkirtIndicesCount <=
            tile.mesh->indices.size()) {
        surfaceIndexOffset =
            static_cast<int>(skirt.noSkirtIndicesBegin * sizeof(uint32_t));
        surfaceIndexCount = static_cast<int>(skirt.noSkirtIndicesCount);
    }

    RenderCommand surfaceCommand = renderer.makeSurfaceTileCommand(
        baseTexture,
        tile.gpuVertexBuffer.get(),
        tile.gpuIndexBuffer.get(),
        surfaceIndexCount);
    surfaceCommand.indexOffset = surfaceIndexOffset;
    surfaceCommand.frameId = frameNumber_;
    surfaceCommand.generation = generation_;
    surfaceCommand.surfaceMeshIndexCount = meshIndexCount;
    surfaceCommand.surfaceNoSkirtIndexCount = surfaceIndexCount;
    surfaceCommand.surfaceSkirtIndexCount =
        std::max(0, meshIndexCount - surfaceIndexCount);
    if (baseBinding.tile) {
        surfaceCommand.surfaceBaseRasterState =
            static_cast<int>(baseBinding.tile->getState());
        surfaceCommand.surfaceBaseIsRectangleTile =
            baseBinding.tile->isRectangleTile() ? 1 : 0;
    }
    surfaceCommand.surfaceTileUv = baseBinding.tile
        ? std::array<float, 4>{
              baseBinding.offsetU,
              baseBinding.offsetV,
              baseBinding.scaleU,
              baseBinding.scaleV}
        : std::array<float, 4>{0.0f, 0.0f, 1.0f, 1.0f};
    if (surfaceClipUv) {
        surfaceCommand.surfaceClipUv = *surfaceClipUv;
        surfaceCommand.surfaceClipEnabled = 1.0f;
    }
    surfaceCommand.surfaceGeometryZoom = tile.key.z;
    surfaceCommand.surfaceTextureZoom = baseBinding.tile
        ? rasterTextureSourceZoom(baseBinding.tile)
        : -1;
    surfaceCommand.surfaceTileOrigin = {
        static_cast<float>(tile.localOrigin.x()),
        static_cast<float>(tile.localOrigin.y()),
        static_cast<float>(tile.localOrigin.z())
    };
    surfaceCommand.surfaceTileOpacity = 1.0f;
    surfaceCommand.surfaceTransitionOpacity = transitionOpacity;
    if (transitionOpacity < 0.999f) {
        surfaceCommand.blend = true;
        surfaceCommand.blendSrc = RenderCommand::BlendFactor::SrcAlpha;
        surfaceCommand.blendDst = RenderCommand::BlendFactorDst::OneMinusSrcAlpha;
    }
    surfaceCommand.surfaceGeneration = static_cast<float>(generation_);

    int overlayTextureCount = 0;
    for (size_t i = 0; i < rasterOverlays_.size() && i < tile.rasterOverlays.size(); ++i) {
        auto* activeOverlay = rasterOverlays_[i];
        if (!activeOverlay || !activeOverlay->visible()) {
            continue;
        }
        if (baseOverlayIndex && *baseOverlayIndex == i) {
            continue;
        }
        const RasterMappedToTilesetTile* mapped =
            tile.rasterOverlays[i].get();
        const SurfaceRasterBinding binding =
            chooseSurfaceRasterBinding(mapped);
        if (!overlayBindingAllowedByPolicy(
                activeOverlay,
                mapped,
                binding)) {
            continue;
        }
        Texture* tex = binding.tile->getTexture();
        if (!tex) continue;

        float uvOffU = binding.offsetU;
        float uvOffV = binding.offsetV;
        float uvScaleU = binding.scaleU;
        float uvScaleV = binding.scaleV;

        if (overlayTextureCount >= kMaxSurfaceImageryOverlays) {
            continue;
        }

        surfaceCommand.textures.push_back(tex);
        surfaceCommand.surfaceOverlayTileUvs[overlayTextureCount] = {
            uvOffU, uvOffV, uvScaleU, uvScaleV};
        surfaceCommand.surfaceOverlayOpacities[overlayTextureCount] =
            activeOverlay->opacity();
        ++overlayTextureCount;
    }

    surfaceCommand.surfaceOverlayTextureCount = overlayTextureCount;
    commands.push_back(std::move(surfaceCommand));
}

void Tileset::buildRenderCommands(Renderer& renderer,
                                   RenderCommandList& commands) {
    const double buildCommandsStartMs = perf::nowMs();
    ++frameNumber_;
    const size_t commandsBeforeTileset = commands.size();

    // cesium-native: set raster overlay provider frame counters BEFORE
    // any tile access so that getTile() stamps tiles with the correct frame.
    for (auto* overlay : rasterOverlays_) {
        if (overlay) {
            overlay->setFrameNumber(frameNumber_);
        }
    }

    double cameraHeight = Ellipsoid::WGS84().cartesianToCartographic(
        lastCameraPosition_).height();
    cameraHeight = std::max(0.0, cameraHeight);
    double fogDensity =
        computeFogDensity(options_.fogDensityTable, cameraHeight);

    int fogCulled = selectedFogCulled_;
    int ensuredTiles = 0;
    int meshReadyTiles = 0;
    int drawAttempts = 0;

    double selectedBuildMs = 0.0;
    double fadeBuildMs = 0.0;
    double detachInactiveMs = 0.0;
    double trimRasterMs = 0.0;
    double eligibilityMs = 0.0;
    double cacheBytesMs = 0.0;
    double unloadMs = 0.0;
    const int synchronousRenderPrepCount =
        tilePlan_.renderEntrySynchronousPrepCount;
    const int deferredRenderPrepCount =
        tilePlan_.renderEntryDeferredPrepCount;
    const int ancestorFallbackDrawCount =
        tilePlan_.renderEntryAncestorFallbackCount;

    auto renderEntry = [&](const TileRenderEntry& entry) {
        TilesetTile* selectedTile = ensureTile(entry.selectedKey);
        if (!selectedTile) return;
        ++ensuredTiles;

        selectedTile->lastUsedFrame = frameNumber_;
        const std::string selectedCk = terrainCacheKey(entry.selectedKey);
        markIneligibleForUnloading(selectedCk);

        TilesetTile* commandTile = ensureTile(entry.renderKey);
        if (!commandTile) return;

        const std::string commandCk = terrainCacheKey(commandTile->key);
        commandTile->lastUsedFrame = frameNumber_;
        // cesium-native: add reference while tile content is in the render list.
        commandTile->addReference();

        // cesium-native: mark tile as ineligible for unloading (used this frame)
        markIneligibleForUnloading(commandCk);

        const size_t before = commands.size();
        if (entry.allowSynchronousMeshPrep) {
            const std::optional<std::array<float, 4>> surfaceClipUv =
                entry.surfaceClipEnabled
                    ? std::optional<std::array<float, 4>>(entry.surfaceClipUv)
                    : std::nullopt;
            buildTileDrawCommand(
                renderer,
                *commandTile,
                commands,
                entry.opacity,
                entry.allowSynchronousMeshPrep,
                surfaceClipUv);
        }
        if (commandTile->meshReady && commandTile->gpuVertexBuffer) {
            ++meshReadyTiles;
        }
        if (commands.size() > before) {
            ++drawAttempts;
        }
    };

    // Build the entries resolved by the cesium-native-style selector contract.
    // Rendering consumes TilePlan::renderEntries; it does not choose fallback
    // ancestors, collapse LOD, or deduplicate traversal results here.
    const double selectedBuildStartMs = perf::nowMs();
    for (const TileRenderEntry& entry : tilePlan_.renderEntries) {
        if (entry.selectedThisFrame) {
            renderEntry(entry);
        }
    }
    selectedBuildMs = perf::nowMs() - selectedBuildStartMs;

    // cesium-native ViewUpdateResult::tilesFadingOut equivalent. These tiles
    // are not selected this frame, but their render content remains referenced
    // until the LOD transition reaches 100%.
    const double fadeBuildStartMs = perf::nowMs();
    for (const TileRenderEntry& entry : tilePlan_.renderEntries) {
        if (!entry.selectedThisFrame) {
            renderEntry(entry);
        }
    }
    fadeBuildMs = perf::nowMs() - fadeBuildStartMs;

    // Drop raster mapping references for geometry tiles that did not contribute
    // to this frame before provider tile trimming.
    const double detachInactiveStartMs = perf::nowMs();
    detachInactiveRasterOverlays(&renderer);
    detachInactiveMs = perf::nowMs() - detachInactiveStartMs;

    // Evict raster overlay tiles that have not been referenced recently.
    const double trimRasterStartMs = perf::nowMs();
    for (auto* overlay : rasterOverlays_) {
        if (overlay) {
            overlay->trimUnusedTiles();
        }
    }
    trimRasterMs = perf::nowMs() - trimRasterStartMs;

    // cesium-native: mark all non-visited tiles as eligible for unloading
    const double eligibilityStartMs = perf::nowMs();
    for (auto& [ck, tile] : tiles_) {
        if (tile->lastUsedFrame != frameNumber_) {
            markEligibleForUnloading(ck);
        }
    }
    eligibilityMs = perf::nowMs() - eligibilityStartMs;

    // cesium-native: recompute total bytes when resources changed. Repeating a
    // full cache walk every frame made settled frames pay for invisible work.
    const double cacheBytesStartMs = perf::nowMs();
    if (cacheBytesDirty_) {
        updateTotalBytesUsed();
        cacheBytesDirty_ = false;
    }
    cacheBytesMs = perf::nowMs() - cacheBytesStartMs;

    // cesium-native: byte-budget-based unload (replaces fixed-tile eviction)
    // Pass &renderer as IPrepareRendererResources for proper detach.
    // Note: removeReference() is called in Scene.cpp AFTER renderer_->submit(),
    // matching cesium-native's reference-counting lifecycle where references
    // are held until the GPU has consumed the commands.
    const double unloadStartMs = perf::nowMs();
    bool hasQueuedUnloadingTile = false;
    for (const std::string& queuedKey : unloadQueue_) {
        auto tileIt = tiles_.find(queuedKey);
        if (tileIt != tiles_.end() &&
            tileIt->second &&
            tileIt->second->loadState == TileLoadState::Unloading) {
            hasQueuedUnloadingTile = true;
            break;
        }
    }
    if (!resourceSmoothingActiveForFrame_ &&
        (totalBytesUsed_ > options_.maximumCachedBytes ||
         hasQueuedUnloadingTile)) {
        unloadCachedBytes(options_.maximumCachedBytes, &renderer);
    }
    unloadMs = perf::nowMs() - unloadStartMs;

    char buildBreakdown[384];
    std::snprintf(
        buildBreakdown,
        sizeof(buildBreakdown),
        "selected=%.2f fade=%.2f detach=%.2f trim=%.2f eligible=%.2f bytes=%.2f unload=%.2f selectedTiles=%zu fadeTiles=%zu ensured=%d cmds=%zu interaction=%d smoothing=%d prepSync=%d prepDeferred=%d fallback=%d",
        selectedBuildMs,
        fadeBuildMs,
        detachInactiveMs,
        trimRasterMs,
        eligibilityMs,
        cacheBytesMs,
        unloadMs,
        tilePlan_.visibleTiles.size(),
        tilePlan_.tilesFadingOut.size(),
        ensuredTiles,
        commands.size() - commandsBeforeTileset,
        interactionActiveForFrame_ ? 1 : 0,
        resourceSmoothingActiveForFrame_ ? 1 : 0,
        synchronousRenderPrepCount,
        deferredRenderPrepCount,
        ancestorFallbackDrawCount);
    perf::logTimingAtLeast(
        frameNumber_,
        "Tileset.buildRenderCommands",
        perf::nowMs() - buildCommandsStartMs,
        20.0,
        buildBreakdown);

#ifndef __ANDROID__
    (void)commandsBeforeTileset;
    (void)fogDensity;
    (void)fogCulled;
    (void)ensuredTiles;
    (void)meshReadyTiles;
    (void)drawAttempts;
#endif
}

void Tileset::releaseRenderReferences() {
    // cesium-native: called from Scene after renderer_->submit().
    // Drops the reference that was added in buildRenderCommands for
    // tiles in the current render list, making them eligible for
    // unloading in the next frame.
    for (auto& [ck, tile] : tiles_) {
        tile->clearReferences();
    }
}

} // namespace earth_engine
