#include "TileSurface.h"
#include "TileScheme.h"
#include "../core/geodesy/Cartographic.h"
#include "../core/geodesy/Ellipsoid.h"
#include "../terrain/TerrainTile.h"

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

bool usesWebMercatorV(const Rectangle& bounds) {
    constexpr double kMaxWebMercatorLat = 1.4844222297453324;
    return bounds.south() >= -kMaxWebMercatorLat &&
           bounds.north() <= kMaxWebMercatorLat;
}

double latitudeForV(const Rectangle& tileBounds, double v) {
    if (!usesWebMercatorV(tileBounds)) {
        return mix(tileBounds.north(), tileBounds.south(), v);
    }

    const double northY = latitudeToMercatorY(tileBounds.north());
    const double southY = latitudeToMercatorY(tileBounds.south());
    return mercatorYToLatitude(mix(northY, southY, v));
}

SurfaceTileSampling samplingForBounds(const Rectangle& bounds) {
    return usesWebMercatorV(bounds)
        ? SurfaceTileSampling::WebMercatorVToWgs84Ecef
        : SurfaceTileSampling::GeographicVToWgs84Ecef;
}

std::pair<Vec3, Vec3> splitHighLow(const Vec3& value) {
    constexpr double kSplit = 65536.0;
    const auto split = [](double v) {
        const double high = std::floor(v / kSplit) * kSplit;
        return std::pair<double, double>{high, v - high};
    };
    const auto sx = split(value.x());
    const auto sy = split(value.y());
    const auto sz = split(value.z());
    return {
        Vec3(sx.first, sy.first, sz.first),
        Vec3(sx.second, sy.second, sz.second)
    };
}

void setPosition(SurfaceVertex& vertex, const Vec3& positionEcef) {
    vertex.positionEcef = positionEcef;
    auto split = splitHighLow(positionEcef);
    vertex.positionHighEcef = split.first;
    vertex.positionLowEcef = split.second;
}

} // namespace

TileSurfaceVertex TileSurface::vertexForUnitUv(const Rectangle& tileBounds,
                                               double u,
                                               double v) {
    const double clampedU = std::clamp(u, 0.0, 1.0);
    const double clampedV = std::clamp(v, 0.0, 1.0);
    const double lng = mix(tileBounds.west(), tileBounds.east(), clampedU);
    const double lat = latitudeForV(tileBounds, clampedV);

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
    const bool mercatorV = usesWebMercatorV(targetBounds) &&
                           usesWebMercatorV(textureBounds);
    const double textureNorthV = mercatorV
        ? latitudeToMercatorY(textureBounds.north())
        : textureBounds.north();
    const double textureSouthV = mercatorV
        ? latitudeToMercatorY(textureBounds.south())
        : textureBounds.south();
    const double height = mercatorV
        ? textureSouthV - textureNorthV
        : textureNorthV - textureSouthV;
    if (width == 0.0 || height == 0.0) return window;

    const double targetNorthV = mercatorV
        ? latitudeToMercatorY(targetBounds.north())
        : targetBounds.north();
    const double targetSouthV = mercatorV
        ? latitudeToMercatorY(targetBounds.south())
        : targetBounds.south();

    window.offsetU = static_cast<float>(
        (targetBounds.west() - textureBounds.west()) / width);
    window.scaleU = static_cast<float>(
        (targetBounds.east() - targetBounds.west()) / width);
    if (mercatorV) {
        window.offsetV = static_cast<float>(
            (targetNorthV - textureNorthV) / height);
        window.scaleV = static_cast<float>(
            (targetSouthV - targetNorthV) / height);
    } else {
        window.offsetV = static_cast<float>(
            (textureNorthV - targetNorthV) / height);
        window.scaleV = static_cast<float>(
            (targetNorthV - targetSouthV) / height);
    }
    return window;
}

SurfaceTileMesh TileSurface::buildEllipsoidMesh(const Rectangle& tileBounds,
                                                int gridSize) {
    const int safeGrid = std::max(1, gridSize);
    const int n = safeGrid + 1;

    SurfaceTileMesh mesh;
    mesh.gridSize = safeGrid;
    mesh.winding = SurfaceTileMeshWinding::Outward;
    mesh.sampling = samplingForBounds(tileBounds);
    mesh.vertices.reserve(static_cast<size_t>(n * n));
    mesh.indices.reserve(static_cast<size_t>(safeGrid * safeGrid * 6));

    for (int y = 0; y < n; ++y) {
        const double v = static_cast<double>(y) / static_cast<double>(safeGrid);
        for (int x = 0; x < n; ++x) {
            const double u = static_cast<double>(x) / static_cast<double>(safeGrid);
            TileSurfaceVertex sampled = vertexForUnitUv(tileBounds, u, v);
            SurfaceVertex vertex;
            setPosition(vertex, sampled.ecef);
            vertex.normalEcef = Ellipsoid::WGS84().geodeticSurfaceNormal(sampled.ecef);
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

SurfaceTileMesh TileSurface::buildTerrainMesh(const Rectangle& tileBounds,
                                              const TerrainTile* terrainTile,
                                              int gridSize,
                                              double skirtHeightMeters,
                                              const TerrainTile* parentTile) {
    const int safeGrid = std::max(1, gridSize);
    const int n = safeGrid + 1;
    const auto& ellipsoid = Ellipsoid::WGS84();

    SurfaceTileMesh mesh;
    mesh.gridSize = safeGrid;
    mesh.winding = SurfaceTileMeshWinding::Outward;
    mesh.sampling = samplingForBounds(tileBounds);
    mesh.vertices.reserve(static_cast<size_t>(n * n));
    mesh.indices.reserve(static_cast<size_t>(safeGrid * safeGrid * 6));

    // OpenGlobus equalizeVertices: blend border vertices with parent tile
    // for smooth transitions between tiles of different resolutions.
    const bool canEqualize = parentTile && parentTile->valid() && terrainTile && terrainTile->valid();

    for (int y = 0; y < n; ++y) {
        const double v = static_cast<double>(y) / static_cast<double>(safeGrid);
        for (int x = 0; x < n; ++x) {
            const double u = static_cast<double>(x) / static_cast<double>(safeGrid);
            TileSurfaceVertex sampled = vertexForUnitUv(tileBounds, u, v);
            Cartographic surfaceCart = ellipsoid.cartesianToCartographic(sampled.ecef);
            double h = terrainTile && terrainTile->valid()
                ? static_cast<double>(terrainTile->sampleHeight(
                    surfaceCart.longitude(), surfaceCart.latitude()))
                : 0.0;

            // OpenGlobus equalizeVertices: use parent tile height for border
            // vertices.  This guarantees that adjacent tiles sharing a border
            // compute identical vertex positions, eliminating seams.
            if (canEqualize) {
                const bool isBorder = (x == 0 || x == n - 1 || y == 0 || y == n - 1);
                if (isBorder) {
                    double parentH = static_cast<double>(parentTile->sampleHeight(
                        surfaceCart.longitude(), surfaceCart.latitude()));
                    if (!parentTile->heightmap()->isNoData(static_cast<float>(parentH))) {
                        h = parentH;  // 100% parent — not blended
                    }
                }
            }

            Cartographic terrainCart = Cartographic::fromRadians(
                surfaceCart.longitude(),
                surfaceCart.latitude(),
                h);

            SurfaceVertex vertex;
            setPosition(vertex, ellipsoid.cartographicToCartesian(terrainCart));
            vertex.normalEcef = Vec3::zero();
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

    for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        SurfaceVertex& a = mesh.vertices[mesh.indices[i]];
        SurfaceVertex& b = mesh.vertices[mesh.indices[i + 1]];
        SurfaceVertex& c = mesh.vertices[mesh.indices[i + 2]];
        const Vec3 faceNormal = (b.positionEcef - a.positionEcef)
            .cross(c.positionEcef - a.positionEcef);
        a.normalEcef += faceNormal;
        b.normalEcef += faceNormal;
        c.normalEcef += faceNormal;
    }
    for (SurfaceVertex& vertex : mesh.vertices) {
        if (vertex.normalEcef.lengthSquared() > 0.0) {
            vertex.normalEcef = vertex.normalEcef.normalized();
        } else {
            vertex.normalEcef = ellipsoid.geodeticSurfaceNormal(vertex.positionEcef);
        }
    }

    if (terrainTile && terrainTile->valid() && skirtHeightMeters < 0.0) {
        auto sampleSkirtHeight = [&](double u, double v) -> double {
            TileSurfaceVertex sampled = vertexForUnitUv(tileBounds, u, v);
            Cartographic surfaceCart = ellipsoid.cartesianToCartographic(sampled.ecef);
            // Skirt top must match the border vertex height for seamless
            // tile joins.  Use parent tile height when equalizing borders.
            if (canEqualize && parentTile) {
                double parentH = static_cast<double>(parentTile->sampleHeight(
                    surfaceCart.longitude(), surfaceCart.latitude()));
                if (!parentTile->heightmap()->isNoData(static_cast<float>(parentH))) {
                    return parentH;
                }
            }
            return static_cast<double>(terrainTile->sampleHeight(
                surfaceCart.longitude(), surfaceCart.latitude()));
        };
        auto appendVertex = [&](double u, double v, double heightOffsetMeters) {
            TileSurfaceVertex sampled = vertexForUnitUv(tileBounds, u, v);
            Cartographic surfaceCart = ellipsoid.cartesianToCartographic(sampled.ecef);
            const double h = sampleSkirtHeight(u, v) + heightOffsetMeters;
            Cartographic cart = Cartographic::fromRadians(
                surfaceCart.longitude(), surfaceCart.latitude(), h);
            SurfaceVertex vertex;
            setPosition(vertex, ellipsoid.cartographicToCartesian(cart));
            vertex.normalEcef = ellipsoid.geodeticSurfaceNormal(cart);
            vertex.uv = sampled.uv;
            mesh.vertices.push_back(vertex);
            return static_cast<uint32_t>(mesh.vertices.size() - 1);
        };

        auto addSkirtEdge = [&](double u0, double v0, double u1, double v1) {
            uint32_t previousTop = 0;
            uint32_t previousBottom = 0;
            for (int i = 0; i <= safeGrid; ++i) {
                const double t = static_cast<double>(i) / static_cast<double>(safeGrid);
                const double u = mix(u0, u1, t);
                const double v = mix(v0, v1, t);
                const uint32_t top = appendVertex(u, v, 0.0);
                const uint32_t bottom = appendVertex(u, v, skirtHeightMeters);
                if (i > 0) {
                    mesh.indices.push_back(previousTop);
                    mesh.indices.push_back(previousBottom);
                    mesh.indices.push_back(top);
                    mesh.indices.push_back(previousBottom);
                    mesh.indices.push_back(bottom);
                    mesh.indices.push_back(top);
                }
                previousTop = top;
                previousBottom = bottom;
            }
        };

        addSkirtEdge(0.0, 0.0, 1.0, 0.0);
        addSkirtEdge(1.0, 0.0, 1.0, 1.0);
        addSkirtEdge(1.0, 1.0, 0.0, 1.0);
        addSkirtEdge(0.0, 1.0, 0.0, 0.0);
    }

    return mesh;
}

SurfaceNormalMap TileSurface::buildNormalMap(const SurfaceTileMesh& mesh) {
    SurfaceNormalMap normalMap;
    if (mesh.gridSize < 1) return normalMap;

    const int n = mesh.gridSize + 1;
    const size_t surfaceVertexCount = static_cast<size_t>(n * n);
    if (mesh.vertices.size() < surfaceVertexCount) return normalMap;

    normalMap.width = n;
    normalMap.height = n;
    normalMap.rgba.resize(surfaceVertexCount * 4);

    auto encode = [](double component) -> uint8_t {
        const double normalized = std::clamp(component * 0.5 + 0.5, 0.0, 1.0);
        return static_cast<uint8_t>(std::round(normalized * 255.0));
    };

    for (size_t i = 0; i < surfaceVertexCount; ++i) {
        Vec3 nrm = mesh.vertices[i].normalEcef;
        if (nrm.lengthSquared() > 0.0) {
            nrm = nrm.normalized();
        } else {
            nrm = Ellipsoid::WGS84().geodeticSurfaceNormal(
                mesh.vertices[i].positionEcef);
        }

        const size_t dst = i * 4;
        normalMap.rgba[dst] = encode(nrm.x());
        normalMap.rgba[dst + 1] = encode(nrm.y());
        normalMap.rgba[dst + 2] = encode(nrm.z());
        normalMap.rgba[dst + 3] = 255;
    }

    return normalMap;
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
