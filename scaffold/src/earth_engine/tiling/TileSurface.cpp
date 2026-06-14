#include "TileSurface.h"
#ifdef __ANDROID__
#include <android/log.h>
#endif
#include "TileScheme.h"
#include "../core/geodesy/Cartographic.h"
#include "../core/geodesy/Ellipsoid.h"
#include "../terrain/QuantizedMeshParser.h"
#include "../terrain/TerrainTile.h"

#include <algorithm>
#include <cmath>
#include <glm/gtc/constants.hpp>

namespace earth_engine {
namespace {

double mix(double a, double b, double t) {
    return a + (b - a) * t;
}

// cesium-native GeographicProjection: projected Y = lat * R (linear).
// All surfaces use linear latitude sampling. WebMercator nonlinearity
// is the imagery provider's responsibility, not the geometry builder's.
double latitudeForV(const Rectangle& tileBounds, double v) {
    return mix(tileBounds.north(), tileBounds.south(), v);
}

SurfaceTileSampling samplingForBounds(const Rectangle& /*bounds*/) {
    return SurfaceTileSampling::GeographicVToWgs84Ecef;
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

// cesium-native: RasterOverlayUtilities::computeTranslationAndScale
// geometryRectangle vs overlayRectangle, both in projected coordinates.
// For EPSG:4326 projected = rad * R, so ratio math is identical to radians.
TileTextureWindow TileSurface::computeTranslationAndScale(
    const Rectangle& geometryBounds,
    const Rectangle& imageryBounds) {
    const double imgWidth = imageryBounds.east() - imageryBounds.west();
    const double imgHeight = imageryBounds.north() - imageryBounds.south();
    if (imgWidth <= 0.0 || imgHeight <= 0.0) return {};

    const double geoWidth = geometryBounds.east() - geometryBounds.west();
    const double geoHeight = geometryBounds.north() - geometryBounds.south();

    TileTextureWindow window;
    window.offsetU = static_cast<float>(
        (geometryBounds.west() - imageryBounds.west()) / imgWidth);
    window.scaleU = static_cast<float>(geoWidth / imgWidth);
    window.offsetV = static_cast<float>(
        (imageryBounds.north() - geometryBounds.north()) / imgHeight);
    window.scaleV = static_cast<float>(geoHeight / imgHeight);
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
                                              const TerrainTile* parentTile,
                                              bool useRawQuantizedMesh) {
    const int safeGrid = std::max(1, gridSize);
    const int n = safeGrid + 1;
    const auto& ellipsoid = Ellipsoid::WGS84();

    // cesium-native path: reconstruct the optimized QuantizedMesh triangulation
    // when raw binary is available. The irregular triangulation preserves mesh
    // decimation quality and border topology better than a regular grid.
    if (useRawQuantizedMesh && terrainTile && terrainTile->valid() &&
        terrainTile->heightmap() && !terrainTile->heightmap()->rawData.empty()) {
        auto qm = QuantizedMeshParser::parseToSurfaceTileMesh(
            terrainTile->heightmap()->rawData.data(),
            terrainTile->heightmap()->rawData.size(),
            tileBounds);
        if (qm) {
#ifdef __ANDROID__
            __android_log_print(ANDROID_LOG_INFO, "TileSurface",
                "QM mesh: verts=%zu idx=%zu skirtVerts=%u",
                qm->vertices.size(), qm->indices.size(),
                qm->skirtMeta.noSkirtVerticesCount);
#endif
            return *qm;
        }
#ifdef __ANDROID__
        __android_log_print(ANDROID_LOG_ERROR, "TileSurface",
            "QM parse failed: rawData=%zu bytes",
            terrainTile->heightmap()->rawData.size());
#endif
        // Fall through to grid-based path on parse failure
    }

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

    // cesium-native alignment: compute per-vertex normals from heightmap gradient
    // (central differences) instead of triangle face averaging. This matches the
    // quality of the old Normal Map texture while using only vertex attributes.
    const double du = 1.0 / static_cast<double>(safeGrid);
    const double dv = 1.0 / static_cast<double>(safeGrid);
    for (int y = 0; y < n; ++y) {
        for (int x = 0; x < n; ++x) {
            SurfaceVertex& vtx = mesh.vertices[static_cast<size_t>(y * n + x)];

            // Sample heights at one-grid-step offsets for gradient
            double hL = 0, hR = 0, hD = 0, hU = 0;
            if (terrainTile && terrainTile->valid()) {
                auto sampleH = [&](double u, double v) -> double {
                    TileSurfaceVertex sv = vertexForUnitUv(tileBounds,
                        std::clamp(u, 0.0, 1.0), std::clamp(v, 0.0, 1.0));
                    Cartographic sc = ellipsoid.cartesianToCartographic(sv.ecef);
                    return static_cast<double>(terrainTile->sampleHeight(
                        sc.longitude(), sc.latitude()));
                };
                const double u = static_cast<double>(x) / safeGrid;
                const double v = static_cast<double>(y) / safeGrid;
                hL = sampleH(u - du, v);
                hR = sampleH(u + du, v);
                hD = sampleH(u, v - dv);
                hU = sampleH(u, v + dv);
            }

            // Build ECEF positions at offsets for gradient
            auto ecefAt = [&](double u, double v, double h) -> Vec3 {
                TileSurfaceVertex sv = vertexForUnitUv(
                    tileBounds, std::clamp(u, 0.0, 1.0), std::clamp(v, 0.0, 1.0));
                Cartographic c = ellipsoid.cartesianToCartographic(sv.ecef);
                return ellipsoid.cartographicToCartesian(
                    Cartographic::fromRadians(c.longitude(), c.latitude(), h));
            };

            const double u = static_cast<double>(x) / safeGrid;
            const double v = static_cast<double>(y) / safeGrid;
            const Vec3 pL = ecefAt(u - du, v, hL);
            const Vec3 pR = ecefAt(u + du, v, hR);
            const Vec3 pD = ecefAt(u, v - dv, hD);
            const Vec3 pU = ecefAt(u, v + dv, hU);

            Vec3 tangentU = pR - pL;
            Vec3 tangentV = pU - pD;
            Vec3 nrm = tangentU.cross(tangentV);

            if (nrm.lengthSquared() > 0.0) {
                vtx.normalEcef = nrm.normalized();
                // Ensure outward-facing
                if (vtx.normalEcef.dot(vtx.positionEcef) < 0.0) {
                    vtx.normalEcef = -vtx.normalEcef;
                }
            } else {
                vtx.normalEcef =
                    ellipsoid.geodeticSurfaceNormal(vtx.positionEcef);
            }
        }
    }

    // cesium-native skirt algorithm.
    // Replaces the old uniform-grid edge walk with sorted edge vertices,
    // dynamic skirt height, and overlap offsets for seamless tile joins.
    //
    // Reference: CesiumQuantizedMeshTerrain/src/QuantizedMeshLoader.cpp
    //   - addSkirt() / addSkirts(): edge-sorted triangle strip construction
    //   - calculateSkirtHeight(): 5.0 × levelMaxGeometricError
    //   - longitudeOffset / latitudeOffset: 0.0001 × tile extent
    if (terrainTile && terrainTile->valid() && skirtHeightMeters < 0.0) {
        // --- cesium-native calcQuadtreeMaxGeometricError ---
        // From CesiumGeospatial/src/calcQuadtreeMaxGeometricError.cpp:
        //   return ellipsoid.getMaximumRadius() * 0.25 / 65.0;
        const double maxGeometricError =
            ellipsoid.semiMajorAxis() * 0.25 / 65.0;

        // Per cesium-native's calculateSkirtHeight():
        //   skirtHeight = 5.0 × calcQuadtreeMaxGeometricError × rectangle.computeWidth()
        const double skirtHeight =
            5.0 * maxGeometricError * tileBounds.width();

        // Overlap offsets for adjacency – 0.0001 × tile angular extent
        const double longitudeOffset =
            (tileBounds.east() - tileBounds.west()) * 0.0001;
        const double latitudeOffset =
            (tileBounds.north() - tileBounds.south()) * 0.0001;

        // Record surface-only ranges before skirt geometry is appended
        mesh.skirtMeta.noSkirtVerticesBegin = 0;
        mesh.skirtMeta.noSkirtVerticesCount =
            static_cast<uint32_t>(mesh.vertices.size());
        mesh.skirtMeta.noSkirtIndicesBegin = 0;
        mesh.skirtMeta.noSkirtIndicesCount =
            static_cast<uint32_t>(mesh.indices.size());

        // Build a skirt edge: given border vertex indices (already in their
        // sorted traversal order), create bottom-of-skirt vertices offset
        // outward and downward, then construct a triangle strip.
        auto addSkirtEdge =
            [&](const std::vector<uint32_t>& edgeIndices, double lonOffset,
                double latOffset) {
                const size_t edgeVerts = edgeIndices.size();
                if (edgeVerts < 2) return;

                const uint32_t firstSkirtVert =
                    static_cast<uint32_t>(mesh.vertices.size());

                for (size_t i = 0; i < edgeVerts; ++i) {
                    const SurfaceVertex& topVert =
                        mesh.vertices[edgeIndices[i]];
                    Cartographic topCart =
                        ellipsoid.cartesianToCartographic(topVert.positionEcef);

                    const double lon = topCart.longitude() + lonOffset;
                    const double lat = topCart.latitude() + latOffset;
                    const double h = topCart.height() - skirtHeight;

                    Cartographic skirtCart =
                        Cartographic::fromRadians(lon, lat, h);
                    Vec3 skirtEcef =
                        ellipsoid.cartographicToCartesian(skirtCart);

                    SurfaceVertex skirtVert;
                    setPosition(skirtVert, skirtEcef);
                    skirtVert.normalEcef =
                        ellipsoid.geodeticSurfaceNormal(skirtEcef);
                    skirtVert.uv = topVert.uv;
                    mesh.vertices.push_back(skirtVert);
                }

                // Triangle strip connecting top edge vertices to skirt
                // bottom vertices.
                for (size_t i = 0; i < edgeVerts - 1; ++i) {
                    const uint32_t topA = edgeIndices[i];
                    const uint32_t topB = edgeIndices[i + 1];
                    const uint32_t skirtA =
                        firstSkirtVert + static_cast<uint32_t>(i);
                    const uint32_t skirtB =
                        firstSkirtVert + static_cast<uint32_t>(i + 1);

                    mesh.indices.push_back(topA);
                    mesh.indices.push_back(topB);
                    mesh.indices.push_back(skirtA);

                    mesh.indices.push_back(skirtA);
                    mesh.indices.push_back(topB);
                    mesh.indices.push_back(skirtB);
                }
            };

        // Edge vertex indices in cesium-native sort order.
        // Grid: y=0 (north), y=safeGrid (south); x=0 (west), x=safeGrid (east).
        // Vertex index = y * n + x.
        //
        // Sort order per cesium-native addSkirts():
        //   West:  by v ASCENDING  → south→north  (y=safeGrid..0)
        //   South: by u DESCENDING → east→west    (x=safeGrid..0)
        //   East:  by v DESCENDING → north→south  (y=0..safeGrid)
        //   North: by u ASCENDING  → west→east    (x=0..safeGrid)

        // West edge: x=0, south→north
        {
            std::vector<uint32_t> westEdge;
            for (int y = safeGrid; y >= 0; --y)
                westEdge.push_back(static_cast<uint32_t>(y * n));
            addSkirtEdge(westEdge, -longitudeOffset, 0.0);
        }
        // South edge: y=safeGrid, east→west
        {
            std::vector<uint32_t> southEdge;
            for (int x = safeGrid; x >= 0; --x)
                southEdge.push_back(static_cast<uint32_t>(safeGrid * n + x));
            addSkirtEdge(southEdge, 0.0, -latitudeOffset);
        }
        // East edge: x=safeGrid, north→south
        {
            std::vector<uint32_t> eastEdge;
            for (int y = 0; y <= safeGrid; ++y)
                eastEdge.push_back(static_cast<uint32_t>(y * n + safeGrid));
            addSkirtEdge(eastEdge, longitudeOffset, 0.0);
        }
        // North edge: y=0, west→east
        {
            std::vector<uint32_t> northEdge;
            for (int x = 0; x <= safeGrid; ++x)
                northEdge.push_back(static_cast<uint32_t>(x));
            addSkirtEdge(northEdge, 0.0, latitudeOffset);
        }
    }

    return mesh;
}

SurfaceNormalMap TileSurface::buildNormalMap(const SurfaceTileMesh& mesh) {
    SurfaceNormalMap normalMap;
    if (mesh.gridSize < 1) return normalMap;

    // Use skirt metadata when available for precise surface vertex count;
    // fall back to grid-size-based count for meshes without skirt metadata.
    const int n = mesh.gridSize + 1;
    const size_t surfaceVertexCount =
        mesh.skirtMeta.noSkirtVerticesCount > 0
            ? static_cast<size_t>(mesh.skirtMeta.noSkirtVerticesCount)
            : static_cast<size_t>(n * n);
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
