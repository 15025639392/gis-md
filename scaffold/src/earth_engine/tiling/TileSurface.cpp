#include "TileSurface.h"
#include "../core/geodesy/QuadtreeGeometricError.h"
#ifdef __ANDROID__
#include <android/log.h>
#endif
#include "TileRasterOverlayDetailsDeriver.h"
#include "TileScheme.h"
#include "../core/geodesy/Cartographic.h"
#include "../core/geodesy/Ellipsoid.h"
#include "../terrain/TerrainTile.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

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
    const double imgWidth = imageryBounds.width();
    const double imgHeight = imageryBounds.height();
    if (imgWidth <= 0.0 || imgHeight <= 0.0) return {};

    const double geoWidth = geometryBounds.width();
    const double geoHeight = geometryBounds.height();
    if (geoWidth <= 0.0 || geoHeight <= 0.0) return {};

    TileTextureWindow window;
    window.offsetU = static_cast<float>(
        (geometryBounds.west() - imageryBounds.west()) / imgWidth);
    window.scaleU = static_cast<float>(geoWidth / imgWidth);
    window.offsetV = static_cast<float>(
        (geometryBounds.south() - imageryBounds.south()) / imgHeight);
    window.scaleV = static_cast<float>(geoHeight / imgHeight);
    return window;
}

TileTextureWindow TileSurface::textureWindowForNorthWestUv(
    const TileTextureWindow& nativeWindow) {
    TileTextureWindow window = nativeWindow;
    window.offsetV = 1.0f - nativeWindow.offsetV - nativeWindow.scaleV;
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
    mesh.rasterOverlayDetails.setGeographicRectangle(tileBounds);
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
    mesh.rasterOverlayDetails.setGeographicRectangle(tileBounds);
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
        // Per cesium-native's calculateSkirtHeight():
        //   skirtHeight = 5.0 × calcQuadtreeMaxGeometricError × rectangle.computeWidth()
        const double skirtHeight =
            calcQuadtreeSkirtHeight(ellipsoid, tileBounds);

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
        Vec3 meshCenter = Vec3::zero();
        for (const SurfaceVertex& vertex : mesh.vertices) {
            meshCenter += vertex.positionEcef;
        }
        mesh.skirtMeta.meshCenter =
            meshCenter / static_cast<double>(mesh.vertices.size());
        mesh.skirtMeta.skirtWestHeight = skirtHeight;
        mesh.skirtMeta.skirtSouthHeight = skirtHeight;
        mesh.skirtMeta.skirtEastHeight = skirtHeight;
        mesh.skirtMeta.skirtNorthHeight = skirtHeight;

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
                    skirtVert.normalEcef = topVert.normalEcef;
                    if (skirtVert.normalEcef.lengthSquared() > 0.0) {
                        skirtVert.normalEcef =
                            skirtVert.normalEcef.normalized();
                    } else {
                        skirtVert.normalEcef =
                            ellipsoid.geodeticSurfaceNormal(skirtEcef);
                    }
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

std::optional<SurfaceTileMesh> TileSurface::upsampleChildMeshFromParent(
    const SurfaceTileMesh& parentMesh,
    const Rectangle& parentBounds,
    const Rectangle& childBounds) {
    if (parentMesh.vertices.empty() || parentMesh.indices.size() < 3) {
        return std::nullopt;
    }

    const double parentWidth = parentBounds.width();
    const double parentHeight = parentBounds.height();
    if (parentWidth <= 0.0 || parentHeight <= 0.0) {
        return std::nullopt;
    }

    auto longitudeOffset = [&](double longitude) {
        double offset = longitude - parentBounds.west();
        if (parentBounds.crossesAntimeridian() && offset < 0.0) {
            offset += MathUtils::TwoPi;
        }
        return offset;
    };

    double uMin = longitudeOffset(childBounds.west()) / parentWidth;
    double uMax = longitudeOffset(childBounds.east()) / parentWidth;
    if (parentBounds.crossesAntimeridian() && uMax < uMin) {
        uMax += 1.0;
    }
    double vMin = (parentBounds.north() - childBounds.north()) / parentHeight;
    double vMax = (parentBounds.north() - childBounds.south()) / parentHeight;

    uMin = std::clamp(uMin, 0.0, 1.0);
    uMax = std::clamp(uMax, 0.0, 1.0);
    vMin = std::clamp(vMin, 0.0, 1.0);
    vMax = std::clamp(vMax, 0.0, 1.0);
    if (uMax <= uMin || vMax <= vMin) {
        return std::nullopt;
    }

    struct ClipVertex {
        SurfaceVertex vertex;
        double u = 0.0;
        double v = 0.0;
    };

    auto interpolate = [](const ClipVertex& a,
                          const ClipVertex& b,
                          double t) {
        t = std::clamp(t, 0.0, 1.0);
        ClipVertex out;
        out.u = mix(a.u, b.u, t);
        out.v = mix(a.v, b.v, t);
        setPosition(
            out.vertex,
            a.vertex.positionEcef +
                (b.vertex.positionEcef - a.vertex.positionEcef) * t);

        Vec3 normal = a.vertex.normalEcef +
                      (b.vertex.normalEcef - a.vertex.normalEcef) * t;
        if (normal.lengthSquared() > 0.0) {
            normal = normal.normalized();
        } else {
            normal = Ellipsoid::WGS84().geodeticSurfaceNormal(
                out.vertex.positionEcef);
        }
        out.vertex.normalEcef = normal;
        out.vertex.uv = {
            static_cast<float>(out.u),
            static_cast<float>(out.v)
        };
        return out;
    };

    auto clipPolygon = [&](const std::vector<ClipVertex>& input,
                           int axis,
                           double threshold,
                           bool keepGreater) {
        std::vector<ClipVertex> output;
        if (input.empty()) return output;
        output.reserve(input.size() + 1);

        auto component = [axis](const ClipVertex& v) {
            return axis == 0 ? v.u : v.v;
        };
        auto inside = [&](const ClipVertex& v) {
            const double c = component(v);
            constexpr double kEpsilon = 1e-12;
            return keepGreater ? c >= threshold - kEpsilon
                               : c <= threshold + kEpsilon;
        };

        ClipVertex previous = input.back();
        bool previousInside = inside(previous);
        for (const ClipVertex& current : input) {
            const bool currentInside = inside(current);
            if (currentInside != previousInside) {
                const double denom = component(current) - component(previous);
                if (std::abs(denom) > 1e-15) {
                    const double t = (threshold - component(previous)) / denom;
                    output.push_back(interpolate(previous, current, t));
                }
            }
            if (currentInside) {
                output.push_back(current);
            }
            previous = current;
            previousInside = currentInside;
        }
        return output;
    };

    SurfaceTileMesh child;
    child.gridSize = 0;
    child.winding = parentMesh.winding;
    child.sampling = parentMesh.sampling;
    child.waterMask = parentMesh.waterMask;
    child.waterMask.scale = parentMesh.waterMask.scale * 0.5;
    const double midLongitude =
        parentBounds.west() + (parentBounds.east() - parentBounds.west()) * 0.5;
    const double midLatitude =
        parentBounds.south() + (parentBounds.north() - parentBounds.south()) * 0.5;
    const double childX = childBounds.west() >= midLongitude ? 1.0 : 0.0;
    const double childY = childBounds.south() >= midLatitude ? 1.0 : 0.0;
    child.waterMask.translationX =
        parentMesh.waterMask.translationX + child.waterMask.scale * childX;
    child.waterMask.translationY =
        parentMesh.waterMask.translationY + child.waterMask.scale * childY;

    uint32_t indexBegin = 0;
    uint32_t safeIndexEnd = static_cast<uint32_t>(parentMesh.indices.size());
    const SkirtMetadata& skirt = parentMesh.skirtMeta;
    if (skirt.noSkirtIndicesCount > 0 &&
        skirt.noSkirtIndicesBegin < parentMesh.indices.size() &&
        skirt.noSkirtIndicesCount <=
            parentMesh.indices.size() - skirt.noSkirtIndicesBegin) {
        indexBegin = skirt.noSkirtIndicesBegin;
        safeIndexEnd = skirt.noSkirtIndicesBegin + skirt.noSkirtIndicesCount;
    }

    double minHeight = std::numeric_limits<double>::max();
    double maxHeight = std::numeric_limits<double>::lowest();
    const auto& ellipsoid = Ellipsoid::WGS84();

    auto appendVertex = [&](ClipVertex vertex) {
        vertex.u = (vertex.u - uMin) / (uMax - uMin);
        vertex.v = (vertex.v - vMin) / (vMax - vMin);
        vertex.u = std::clamp(vertex.u, 0.0, 1.0);
        vertex.v = std::clamp(vertex.v, 0.0, 1.0);
        vertex.vertex.uv = {
            static_cast<float>(vertex.u),
            static_cast<float>(vertex.v)
        };
        setPosition(vertex.vertex, vertex.vertex.positionEcef);
        if (vertex.vertex.normalEcef.lengthSquared() > 0.0) {
            vertex.vertex.normalEcef = vertex.vertex.normalEcef.normalized();
        } else {
            vertex.vertex.normalEcef =
                ellipsoid.geodeticSurfaceNormal(vertex.vertex.positionEcef);
        }

        const Cartographic cart =
            ellipsoid.cartesianToCartographic(vertex.vertex.positionEcef);
        minHeight = std::min(minHeight, cart.height());
        maxHeight = std::max(maxHeight, cart.height());

        child.vertices.push_back(vertex.vertex);
        return static_cast<uint32_t>(child.vertices.size() - 1);
    };

    for (uint32_t i = indexBegin; i + 2 < safeIndexEnd; i += 3) {
        const uint32_t ia = parentMesh.indices[i];
        const uint32_t ib = parentMesh.indices[i + 1];
        const uint32_t ic = parentMesh.indices[i + 2];
        if (ia >= parentMesh.vertices.size() ||
            ib >= parentMesh.vertices.size() ||
            ic >= parentMesh.vertices.size()) {
            continue;
        }

        std::vector<ClipVertex> polygon = {
            ClipVertex{parentMesh.vertices[ia],
                       parentMesh.vertices[ia].uv[0],
                       parentMesh.vertices[ia].uv[1]},
            ClipVertex{parentMesh.vertices[ib],
                       parentMesh.vertices[ib].uv[0],
                       parentMesh.vertices[ib].uv[1]},
            ClipVertex{parentMesh.vertices[ic],
                       parentMesh.vertices[ic].uv[0],
                       parentMesh.vertices[ic].uv[1]}
        };

        polygon = clipPolygon(polygon, 0, uMin, true);
        polygon = clipPolygon(polygon, 0, uMax, false);
        polygon = clipPolygon(polygon, 1, vMin, true);
        polygon = clipPolygon(polygon, 1, vMax, false);
        if (polygon.size() < 3) {
            continue;
        }

        const uint32_t base = appendVertex(polygon[0]);
        uint32_t previous = appendVertex(polygon[1]);
        for (size_t p = 2; p < polygon.size(); ++p) {
            const uint32_t current = appendVertex(polygon[p]);
            child.indices.push_back(base);
            child.indices.push_back(previous);
            child.indices.push_back(current);
            previous = current;
        }
    }

    if (child.vertices.empty() || child.indices.empty()) {
        return std::nullopt;
    }

    child.skirtMeta.noSkirtVerticesBegin = 0;
    child.skirtMeta.noSkirtVerticesCount =
        static_cast<uint32_t>(child.vertices.size());
    child.skirtMeta.noSkirtIndicesBegin = 0;
    child.skirtMeta.noSkirtIndicesCount =
        static_cast<uint32_t>(child.indices.size());
    child.hasHeightRange = true;
    child.minimumHeight = minHeight;
    child.maximumHeight = maxHeight;
    child.rasterOverlayDetails =
        TileRasterOverlayDetailsDeriver::deriveChildFromParent(
        parentMesh.rasterOverlayDetails,
        parentBounds,
        childBounds,
        minHeight,
        maxHeight);
    return child;
}

SurfaceNormalMap TileSurface::buildNormalMap(const SurfaceTileMesh& mesh) {
    SurfaceNormalMap normalMap;
    if (mesh.gridSize < 1) return normalMap;

    // Use skirt metadata when available for precise surface vertex count;
    // fall back to grid-size-based count for meshes without skirt metadata.
    const int n = mesh.gridSize + 1;
    size_t surfaceVertexBegin = 0;
    size_t surfaceVertexCount = static_cast<size_t>(n * n);
    const SkirtMetadata& skirt = mesh.skirtMeta;
    if (skirt.noSkirtVerticesCount > 0 &&
        skirt.noSkirtVerticesBegin < mesh.vertices.size() &&
        skirt.noSkirtVerticesCount <=
            mesh.vertices.size() - skirt.noSkirtVerticesBegin) {
        surfaceVertexBegin = skirt.noSkirtVerticesBegin;
        surfaceVertexCount = skirt.noSkirtVerticesCount;
    }
    if (mesh.vertices.size() < surfaceVertexBegin + surfaceVertexCount) {
        return normalMap;
    }

    normalMap.width = n;
    normalMap.height = n;
    normalMap.rgba.resize(surfaceVertexCount * 4);

    auto encode = [](double component) -> uint8_t {
        const double normalized = std::clamp(component * 0.5 + 0.5, 0.0, 1.0);
        return static_cast<uint8_t>(std::round(normalized * 255.0));
    };

    for (size_t i = 0; i < surfaceVertexCount; ++i) {
        const SurfaceVertex& vertex = mesh.vertices[surfaceVertexBegin + i];
        Vec3 nrm = vertex.normalEcef;
        if (nrm.lengthSquared() > 0.0) {
            nrm = nrm.normalized();
        } else {
            nrm = Ellipsoid::WGS84().geodeticSurfaceNormal(
                vertex.positionEcef);
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
