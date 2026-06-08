#include "TileSurface.h"
#include "TileScheme.h"
#include "../core/geodesy/Cartographic.h"
#include "../core/geodesy/Ellipsoid.h"

#include <algorithm>
#include <cmath>
#include <glm/gtc/constants.hpp>

namespace earth_engine {
namespace {

double mix(double a, double b, double t) {
    return a + (b - a) * t;
}

double latitudeToMercatorY(double latRad) {
    constexpr double kMaxWebMercatorLat = 1.4844222297453324;
    const double lat = std::clamp(latRad, -kMaxWebMercatorLat, kMaxWebMercatorLat);
    return (glm::pi<double>() -
            std::log(std::tan(lat * 0.5 + glm::quarter_pi<double>()))) /
           glm::two_pi<double>();
}

double mercatorYToLatitude(double y) {
    return std::atan(std::sinh(glm::pi<double>() - glm::two_pi<double>() * y));
}

} // namespace

TileSurfaceVertex TileSurface::vertexForUnitUv(const Rectangle& tileBounds,
                                               double u,
                                               double v) {
    const double clampedU = std::clamp(u, 0.0, 1.0);
    const double clampedV = std::clamp(v, 0.0, 1.0);
    const double lng = mix(tileBounds.west(), tileBounds.east(), clampedU);
    const double northY = latitudeToMercatorY(tileBounds.north());
    const double southY = latitudeToMercatorY(tileBounds.south());
    const double lat = mercatorYToLatitude(mix(northY, southY, clampedV));

    TileSurfaceVertex vertex;
    vertex.ecef = Ellipsoid::WGS84().cartographicToCartesian(
        Cartographic::fromRadians(lng, lat, 0.0));
    vertex.uv = {
        static_cast<float>(clampedU),
        static_cast<float>(clampedV)
    };
    return vertex;
}

TileTextureWindow TileSurface::textureWindow(const Rectangle& targetBounds,
                                             const Rectangle& textureBounds) {
    TileTextureWindow window;
    const double width = textureBounds.east() - textureBounds.west();
    const double textureNorthY = latitudeToMercatorY(textureBounds.north());
    const double textureSouthY = latitudeToMercatorY(textureBounds.south());
    const double height = textureSouthY - textureNorthY;
    if (width == 0.0 || height == 0.0) return window;

    const double targetNorthY = latitudeToMercatorY(targetBounds.north());
    const double targetSouthY = latitudeToMercatorY(targetBounds.south());

    window.offsetU = static_cast<float>(
        (targetBounds.west() - textureBounds.west()) / width);
    window.scaleU = static_cast<float>(
        (targetBounds.east() - targetBounds.west()) / width);
    window.offsetV = static_cast<float>(
        (targetNorthY - textureNorthY) / height);
    window.scaleV = static_cast<float>(
        (targetSouthY - targetNorthY) / height);
    return window;
}

SurfaceTileMesh TileSurface::buildEllipsoidMesh(const Rectangle& tileBounds,
                                                int gridSize) {
    const int safeGrid = std::max(1, gridSize);
    const int n = safeGrid + 1;

    SurfaceTileMesh mesh;
    mesh.gridSize = safeGrid;
    mesh.winding = SurfaceTileMeshWinding::Outward;
    mesh.sampling = SurfaceTileSampling::WebMercatorVToWgs84Ecef;
    mesh.vertices.reserve(static_cast<size_t>(n * n));
    mesh.indices.reserve(static_cast<size_t>(safeGrid * safeGrid * 6));

    for (int y = 0; y < n; ++y) {
        const double v = static_cast<double>(y) / static_cast<double>(safeGrid);
        for (int x = 0; x < n; ++x) {
            const double u = static_cast<double>(x) / static_cast<double>(safeGrid);
            TileSurfaceVertex sampled = vertexForUnitUv(tileBounds, u, v);
            SurfaceVertex vertex;
            vertex.positionEcef = sampled.ecef;
            vertex.normalEcef = sampled.ecef.normalized();
            vertex.uv = sampled.uv;
            mesh.vertices.push_back(vertex);
        }
    }

    for (int y = 0; y < safeGrid; ++y) {
        for (int x = 0; x < safeGrid; ++x) {
            const uint32_t a = static_cast<uint32_t>(y * n + x);
            const uint32_t b = static_cast<uint32_t>(y * n + x + 1);
            const uint32_t c = static_cast<uint32_t>((y + 1) * n + x);
            const uint32_t d = static_cast<uint32_t>((y + 1) * n + x + 1);
            mesh.indices.push_back(a);
            mesh.indices.push_back(c);
            mesh.indices.push_back(b);
            mesh.indices.push_back(b);
            mesh.indices.push_back(c);
            mesh.indices.push_back(d);
        }
    }

    return mesh;
}

bool TileSurface::trianglesFaceOutward(const Rectangle& tileBounds) {
    const Vec3 a = vertexForUnitUv(tileBounds, 0.0, 0.0).ecef;
    const Vec3 b = vertexForUnitUv(tileBounds, 1.0, 0.0).ecef;
    const Vec3 c = vertexForUnitUv(tileBounds, 0.0, 1.0).ecef;
    const Vec3 d = vertexForUnitUv(tileBounds, 1.0, 1.0).ecef;

    const Vec3 n0 = (c - a).cross(b - a);
    const Vec3 center0 = (a + c + b) / 3.0;
    const Vec3 n1 = (c - b).cross(d - b);
    const Vec3 center1 = (b + c + d) / 3.0;
    return n0.dot(center0) > 0.0 && n1.dot(center1) > 0.0;
}

Rectangle TileSurface::boundsForKey(const TileScheme& scheme, const TileKey& key) {
    return scheme.tileToRectangle(key);
}

} // namespace earth_engine
