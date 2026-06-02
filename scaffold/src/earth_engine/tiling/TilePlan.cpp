#include "TilePlan.h"
#include "TileScheme.h"
#include "../scene/Camera.h"
#include "../core/geodesy/Ellipsoid.h"
#include "../core/geodesy/Cartographic.h"
#include "../core/math/Vec3.h"
#include "../core/math/Rectangle.h"
#include "../core/math/Ray.h"

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <cmath>
#include <algorithm>
#include <limits>
#include <unordered_set>

namespace earth_engine {

namespace {

constexpr double kEarthRadius = 6378137.0;
constexpr double kWgs84SemiMinor = 6356752.3142451793;
constexpr double kMaxWebMercatorLat = 1.4844222297453324; // 85.05112878 deg

double normalizeLongitude(double lngRad) {
    double x = std::fmod(lngRad + glm::pi<double>(), glm::two_pi<double>());
    if (x < 0.0) x += glm::two_pi<double>();
    return x - glm::pi<double>();
}

double longitudeToPositive(double lngRad) {
    double x = std::fmod(normalizeLongitude(lngRad) + glm::two_pi<double>(),
                         glm::two_pi<double>());
    if (x < 0.0) x += glm::two_pi<double>();
    return x;
}

double positiveToLongitude(double lngRad) {
    return normalizeLongitude(lngRad);
}

bool rayEllipsoidIntersection(const Ray& ray, Vec3& hit) {
    const Vec3& origin = ray.origin();
    const Vec3& direction = ray.direction();

    // Scale WGS84 ellipsoid to a sphere of radius a.
    const double s = kEarthRadius / kWgs84SemiMinor;
    const double ox = origin.x();
    const double oy = origin.y();
    const double oz = origin.z() * s;
    const double dx = direction.x();
    const double dy = direction.y();
    const double dz = direction.z() * s;

    const double a = dx * dx + dy * dy + dz * dz;
    const double b = 2.0 * (ox * dx + oy * dy + oz * dz);
    const double c = ox * ox + oy * oy + oz * oz - kEarthRadius * kEarthRadius;

    const double discriminant = b * b - 4.0 * a * c;
    if (discriminant < 0.0) return false;

    const double sqrtD = std::sqrt(discriminant);
    const double t0 = (-b - sqrtD) / (2.0 * a);
    const double t1 = (-b + sqrtD) / (2.0 * a);

    double t = std::numeric_limits<double>::infinity();
    if (t0 > 0.0) t = t0;
    else if (t1 > 0.0) t = t1;
    if (!std::isfinite(t)) return false;

    hit = ray.pointAt(t);
    return true;
}

Rectangle rectangleFromPoints(const std::vector<Cartographic>& points, int zoom) {
    double south = glm::half_pi<double>();
    double north = -glm::half_pi<double>();
    std::vector<double> lngs;
    lngs.reserve(points.size());

    for (const auto& p : points) {
        lngs.push_back(longitudeToPositive(p.longitude()));
        south = std::min(south, p.latitude());
        north = std::max(north, p.latitude());
    }

    std::sort(lngs.begin(), lngs.end());

    double west = -glm::pi<double>();
    double east = glm::pi<double>();
    if (!lngs.empty()) {
        double largestGap = -1.0;
        size_t gapStart = 0;
        for (size_t i = 0; i < lngs.size(); ++i) {
            const double a = lngs[i];
            const double b = (i + 1 < lngs.size())
                ? lngs[i + 1]
                : lngs[0] + glm::two_pi<double>();
            const double gap = b - a;
            if (gap > largestGap) {
                largestGap = gap;
                gapStart = i;
            }
        }

        const double covered = glm::two_pi<double>() - largestGap;
        if (covered < glm::two_pi<double>() - 1e-9) {
            const double westPos = lngs[(gapStart + 1) % lngs.size()];
            const double eastPos = lngs[gapStart];
            west = positiveToLongitude(westPos);
            east = positiveToLongitude(eastPos);
        }
    }

    // One tile-ish padding absorbs sample-grid misses near the limb/frustum edge.
    const double pad = std::max(glm::radians(0.5),
                                glm::two_pi<double>() /
                                    static_cast<double>(1 << std::max(0, zoom)));
    south = std::clamp(south - pad, -kMaxWebMercatorLat, kMaxWebMercatorLat);
    north = std::clamp(north + pad, -kMaxWebMercatorLat, kMaxWebMercatorLat);

    if (west != -glm::pi<double>() || east != glm::pi<double>()) {
        west = normalizeLongitude(west - pad);
        east = normalizeLongitude(east + pad);
    }

    return Rectangle(west, south, east, north);
}

std::vector<Cartographic> sampleVisiblePoints(const Camera& camera,
                                              const Ellipsoid& ellipsoid,
                                              double viewportWidthPixels,
                                              double viewportHeightPixels) {
    std::vector<Cartographic> points;
    // Include edges and interior; this captures partial globes where corners miss.
    constexpr int kGrid = 12;
    points.reserve((kGrid + 1) * (kGrid + 1));

    for (int y = 0; y <= kGrid; ++y) {
        for (int x = 0; x <= kGrid; ++x) {
            const double sx = viewportWidthPixels *
                static_cast<double>(x) / static_cast<double>(kGrid);
            const double sy = viewportHeightPixels *
                static_cast<double>(y) / static_cast<double>(kGrid);

            Vec3 hit;
            if (rayEllipsoidIntersection(
                    camera.getPickRay(sx, sy, viewportWidthPixels, viewportHeightPixels),
                    hit)) {
                points.push_back(ellipsoid.cartesianToCartographic(hit));
            }
        }
    }

    // Add the analytic horizon cap. Viewport samples capture the actual frustum
    // footprint; the cap prevents sparse sampling from missing the globe limb.
    Cartographic center = ellipsoid.cartesianToCartographic(
        camera.position().normalized() * kEarthRadius);
    points.push_back(center);

    const double cameraDistance = camera.position().length();
    if (cameraDistance > kEarthRadius) {
        const double capRadius = std::acos(
            std::clamp(kEarthRadius / cameraDistance, 0.0, 1.0));
        const double sinLat = std::sin(center.latitude());
        const double cosLat = std::cos(center.latitude());
        const double sinD = std::sin(capRadius);
        const double cosD = std::cos(capRadius);

        constexpr int kCapSamples = 48;
        for (int i = 0; i < kCapSamples; ++i) {
            const double bearing = glm::two_pi<double>() *
                static_cast<double>(i) / static_cast<double>(kCapSamples);
            const double sinLat2 =
                sinLat * cosD + cosLat * sinD * std::cos(bearing);
            const double lat2 = std::asin(std::clamp(sinLat2, -1.0, 1.0));
            const double lon2 = center.longitude() + std::atan2(
                std::sin(bearing) * sinD * cosLat,
                cosD - sinLat * std::sin(lat2));
            points.push_back(Cartographic::fromRadians(
                normalizeLongitude(lon2), lat2, 0.0));
        }
    }

    return points;
}

std::vector<Rectangle> splitForTileRange(const Rectangle& rect) {
    if (!rect.crossesAntimeridian()) return {rect};
    return {
        Rectangle(rect.west(), rect.south(), glm::pi<double>(), rect.north()),
        Rectangle(-glm::pi<double>(), rect.south(), rect.east(), rect.north())
    };
}

void appendTileRange(const TileScheme& scheme,
                     const Rectangle& rect,
                     int zoom,
                     std::vector<TileKey>& out,
                     std::unordered_set<TileKey>& seen) {
    int minX = 0, minY = 0, maxX = 0, maxY = 0;
    scheme.tileRange(rect, zoom, minX, minY, maxX, maxY);

    const int tilesAtZoom = 1 << zoom;
    for (int y = minY; y <= maxY; ++y) {
        if (y < 0 || y >= tilesAtZoom) continue;
        for (int x = minX; x <= maxX; ++x) {
            int wrappedX = x % tilesAtZoom;
            if (wrappedX < 0) wrappedX += tilesAtZoom;
            TileKey key{scheme.id(), zoom, wrappedX, y};
            if (seen.insert(key).second) {
                out.push_back(std::move(key));
            }
        }
    }
}

std::vector<TileKey> buildParentTiles(const TileScheme& scheme,
                                      const std::vector<TileKey>& visibleTiles,
                                      int zoom) {
    std::vector<TileKey> parents;
    if (zoom <= scheme.minZoom()) return parents;

    std::unordered_set<TileKey> seen;
    parents.reserve(visibleTiles.size() / 4 + 1);
    const int parentZoom = zoom - 1;
    const int parentTilesAtZoom = 1 << parentZoom;

    for (const auto& key : visibleTiles) {
        TileKey parent{
            scheme.id(),
            parentZoom,
            std::clamp(key.x / 2, 0, parentTilesAtZoom - 1),
            std::clamp(key.y / 2, 0, parentTilesAtZoom - 1)
        };
        if (seen.insert(parent).second) {
            parents.push_back(std::move(parent));
        }
    }
    return parents;
}

} // namespace

TilePlan TilePlanBuilder::compute(const Camera& camera,
                                   const TileScheme& scheme,
                                   double viewportWidthPixels,
                                   double viewportHeightPixels,
                                   int previousZoom) {
    TilePlan plan;

    const auto& ellipsoid = Ellipsoid::WGS84();

    // 1. 计算相机距地球中心距离
    Vec3 camPos = camera.position();
    double cameraDistance = camPos.length();

    // 2. 估算相机距地表的平均高度
    double cameraHeight = cameraDistance - kEarthRadius;
    if (cameraHeight < 1000.0) cameraHeight = 1000.0;  // 最低 1km

    // 3. 确定 zoom 层级
    int zoom = zoomLevelFromHeight(cameraHeight,
                                   viewportHeightPixels,
                                   camera.verticalFovRadians(),
                                   scheme.tileSize(),
                                   scheme.minZoom(),
                                   scheme.maxZoom(),
                                   previousZoom);
    plan.zoom = zoom;

    // At low zoom the whole WebMercator world is tiny (z=3 => 64 tiles).
    // Rendering the complete matrix avoids visible globe holes caused by
    // horizon/footprint approximations while keeping request cost bounded.
    if (zoom <= 3) {
        const int tilesAtZoom = 1 << zoom;
        plan.visibleTiles.reserve(static_cast<size_t>(tilesAtZoom * tilesAtZoom));
        for (int y = 0; y < tilesAtZoom; ++y) {
            for (int x = 0; x < tilesAtZoom; ++x) {
                plan.visibleTiles.push_back(TileKey{scheme.id(), zoom, x, y});
            }
        }
        plan.parentTiles = buildParentTiles(scheme, plan.visibleTiles, zoom);
        return plan;
    }

    // 4. Sample the actual visible ellipsoid footprint from the viewport.
    const auto visiblePoints = sampleVisiblePoints(
        camera, ellipsoid, viewportWidthPixels, viewportHeightPixels);
    if (visiblePoints.empty()) {
        return plan;
    }

    Rectangle visibleRect = rectangleFromPoints(visiblePoints, zoom);

    // 5. Convert footprint rectangles to tile keys. Antimeridian-crossing
    // rectangles are split so tileRange never interprets them as a huge span.
    std::unordered_set<TileKey> seen;
    for (const Rectangle& rect : splitForTileRange(visibleRect)) {
        appendTileRange(scheme, rect, zoom, plan.visibleTiles, seen);
    }

    // 6. Parent fallback follows the exact child set, not the broad rectangle.
    plan.parentTiles = buildParentTiles(scheme, plan.visibleTiles, zoom);

    return plan;
}

int TilePlanBuilder::zoomLevelFromHeight(double cameraHeightMeters,
                                          double viewportHeightPixels,
                                          double verticalFovRadians,
                                          int tileSize,
                                          int minZoom,
                                          int maxZoom,
                                          int previousZoom) {
    if (viewportHeightPixels <= 0.0) return minZoom;

    constexpr double kEarthCircumference = 2.0 * glm::pi<double>() * 6378137.0;

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

} // namespace earth_engine
