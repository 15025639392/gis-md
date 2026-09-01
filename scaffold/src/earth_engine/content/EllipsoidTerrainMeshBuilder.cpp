#include "EllipsoidTerrainMeshBuilder.h"

#include "GltfModel.h"
#include "../core/geodesy/Cartographic.h"
#include "../core/geodesy/Ellipsoid.h"
#include "../core/geodesy/QuadtreeGeometricError.h"
#include "../core/math/MathUtils.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace earth_engine {
std::unique_ptr<GltfModel> makeEllipsoidModel(
    const Rectangle& geographicRectangle,
    const std::vector<RasterOverlayProjection>& projections,
    int gridSize,
    const EllipsoidProxyHeightSampler& heightSampler,
    bool computeGridNormals,
    bool computeGeomorphDelta,
    bool buildSkirt);

namespace {

constexpr double kEllipsoidTerrainMinimumHeight = 0.0;
constexpr double kEllipsoidTerrainMaximumHeight = 0.0;

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
        Vec3(sx.second, sy.second, sz.second)};
}

void setLocalPosition(SurfaceVertex& vertex, const Vec3& localPosition) {
    vertex.positionEcef = localPosition;
    const auto split = splitHighLow(localPosition);
    vertex.positionHighEcef = split.first;
    vertex.positionLowEcef = split.second;
}

Rectangle projectRasterRectangle(const Rectangle& geographicRectangle,
                                 RasterOverlayProjection projection) {
    return projectWorldRectangleForRasterOverlay(
        geographicRectangle,
        projection);
}

void rewriteProjectionTexCoords(GltfPrimitive& primitive,
                                RasterOverlayProjection projection,
                                const Rectangle& geographicRectangle,
                                size_t textureCoordinateIndex) {
    if (textureCoordinateIndex >= kGltfMaxTexCoordSets) {
        return;
    }
    std::vector<std::array<float, 2>>& texCoords =
        primitive.vertexTexCoords[textureCoordinateIndex];
    texCoords.clear();
    texCoords.reserve(primitive.vertices.size());
    const Rectangle projectedRectangle =
        projectRasterRectangle(geographicRectangle, projection);
    const double width = projectedRectangle.width();
    const double height = projectedRectangle.height();
    const Ellipsoid& ellipsoid = Ellipsoid::WGS84();
    for (const SurfaceVertex& vertex : primitive.vertices) {
        const std::optional<Cartographic> cartographic =
            ellipsoid.tryCartesianToCartographic(vertex.positionEcef);
        if (!cartographic || width <= 0.0 || height <= 0.0) {
            texCoords.push_back({0.0f, 0.0f});
            continue;
        }
        const Vec3 projected =
            projectWorldPositionForRasterOverlay(*cartographic, projection);
        double projectedX = projected.x();
        if (projection == RasterOverlayProjection::Geographic &&
            projectedRectangle.crossesAntimeridian() &&
            projectedX < projectedRectangle.west()) {
            projectedX += MathUtils::TwoPi;
        }
        texCoords.push_back({
            static_cast<float>(std::clamp(
                (projectedX - projectedRectangle.west()) / width,
                0.0,
                1.0)),
            // NW 约定（v=0 在北），与纹理行序及 UV 窗口换算一致
            static_cast<float>(std::clamp(
                (projectedRectangle.north() - projected.y()) / height,
                0.0,
                1.0))});
    }
}

double mixValue(double a, double b, double t) {
    return a + (b - a) * t;
}

double longitudeAt(const Rectangle& rectangle, double t) {
    double east = rectangle.east();
    if (rectangle.crossesAntimeridian()) {
        east += MathUtils::TwoPi;
    }
    double longitude = mixValue(rectangle.west(), east, t);
    if (longitude > MathUtils::OnePi) {
        longitude -= MathUtils::TwoPi;
    }
    return longitude;
}

// Regular ellipsoid-surface grid over the geographic rectangle. Vertices carry
// absolute ECEF positions (high/low split), geodetic surface normals, and
// unit-UV texture coordinates. Index winding (a, c, b, b, c, d).
void buildEllipsoidGrid(const Rectangle& tileBounds,
                        int gridSize,
                        const EllipsoidProxyHeightSampler& heightSampler,
                        bool computeGridNormals,
                        bool computeGeomorphDelta,
                        std::vector<SurfaceVertex>& outVertices,
                        std::vector<uint32_t>& outIndices) {
    const int safeGrid = std::max(1, gridSize);
    const int n = safeGrid + 1;
    const Ellipsoid& ellipsoid = Ellipsoid::WGS84();

    outVertices.clear();
    outIndices.clear();
    outVertices.reserve(static_cast<size_t>(n * n));
    outIndices.reserve(static_cast<size_t>(safeGrid * safeGrid * 6));

    // Sampled height per vertex (row-major), kept so the geomorph coarse-self
    // downsample below reads them directly instead of re-projecting positions.
    std::vector<double> gridHeights;
    gridHeights.reserve(static_cast<size_t>(n) * n);

    for (int y = 0; y < n; ++y) {
        const double v = static_cast<double>(y) / static_cast<double>(safeGrid);
        const double clampedV = std::clamp(v, 0.0, 1.0);
        // Linear latitude sampling: north at v=0, south at v=1.
        const double lat = mixValue(tileBounds.north(), tileBounds.south(),
                                    clampedV);
        for (int x = 0; x < n; ++x) {
            const double u =
                static_cast<double>(x) / static_cast<double>(safeGrid);
            const double clampedU = std::clamp(u, 0.0, 1.0);
            const double lng = longitudeAt(tileBounds, clampedU);
            // Borrow the loaded-terrain height at this lon/lat (edges shared
            // with loaded neighbours meet crack-free); fall back to the
            // ellipsoid surface (0) where no terrain is loaded. Height does not
            // affect lon/lat, so overlay UVs are unchanged.
            double height = 0.0;
            if (heightSampler) {
                const std::optional<float> sampled = heightSampler(lng, lat);
                if (sampled) {
                    height = static_cast<double>(*sampled);
                }
            }
            const Vec3 ecef = ellipsoid.cartographicToCartesian(
                Cartographic::fromRadians(lng, lat, height));

            SurfaceVertex vertex;
            setLocalPosition(vertex, ecef);
            vertex.normalEcef = ellipsoid.geodeticSurfaceNormal(ecef);
            vertex.uv = {
                static_cast<float>(clampedU),
                static_cast<float>(clampedV)};
            outVertices.push_back(vertex);
            gridHeights.push_back(height);
        }
    }

    // Real-terrain slope normals: central difference of neighbouring grid
    // heights (one-sided at tile edges). Overwrites the flat geodetic normal set
    // above. East×North = outward up in the local ENU frame.
    if (computeGridNormals) {
        const auto posAt = [&](int gx, int gy) -> const Vec3& {
            return outVertices[static_cast<size_t>(gy * n + gx)].positionEcef;
        };
        for (int y = 0; y < n; ++y) {
            for (int x = 0; x < n; ++x) {
                const Vec3 tangentEast =
                    posAt(std::min(x + 1, n - 1), y) - posAt(std::max(x - 1, 0), y);
                const Vec3 tangentNorth =
                    posAt(x, std::max(y - 1, 0)) - posAt(x, std::min(y + 1, n - 1));
                const Vec3 normal = tangentEast.cross(tangentNorth);
                const double len = normal.length();
                SurfaceVertex& vertex =
                    outVertices[static_cast<size_t>(y * n + x)];
                vertex.normalEcef = len > 1e-6
                    ? Vec3(normal.x() / len, normal.y() / len, normal.z() / len)
                    : ellipsoid.geodeticSurfaceNormal(vertex.positionEcef);
            }
        }
    }

    // Geomorph coarse-self delta (osgEarth neighbour-average, baked once here in
    // the decode worker → zero main-thread cost, no parent-tile dependency).
    // coarse-self = bilinear of the 2× (even-index) subgrid at (x,y):
    //   even/even → self (a coarse grid point, delta 0),
    //   odd  col  → ½(left even + right even),  odd row → ½(top + bottom),
    //   odd/odd   → ¼(4 diagonal even neighbours).
    // heightDelta = coarse − true, so at morph=0 the vertex sits on the coarse
    // (parent-like) surface and morphs up to true detail as morph→1. Requires an
    // even gridSize (n odd) so the even subgrid spans both edges and adjacent
    // tiles agree on shared boundary rows/cols (crack-free at any single morph).
    if (computeGeomorphDelta && safeGrid % 2 == 0) {
        const auto heightAt = [&](int gx, int gy) -> double {
            return gridHeights[static_cast<size_t>(gy) * n + gx];
        };
        for (int y = 0; y < n; ++y) {
            const int y0 = (y / 2) * 2;
            const int y1 = std::min(y0 + 2, n - 1);
            const double fy = static_cast<double>(y - y0) * 0.5;
            for (int x = 0; x < n; ++x) {
                const int x0 = (x / 2) * 2;
                const int x1 = std::min(x0 + 2, n - 1);
                const double fx = static_cast<double>(x - x0) * 0.5;
                const double top =
                    heightAt(x0, y0) * (1.0 - fx) + heightAt(x1, y0) * fx;
                const double bottom =
                    heightAt(x0, y1) * (1.0 - fx) + heightAt(x1, y1) * fx;
                const double coarse = top * (1.0 - fy) + bottom * fy;
                outVertices[static_cast<size_t>(y) * n + x].geomorphHeightDelta =
                    static_cast<float>(coarse - heightAt(x, y));
            }
        }
    }

    for (int y = 0; y < safeGrid; ++y) {
        for (int x = 0; x < safeGrid; ++x) {
            const uint32_t a = static_cast<uint32_t>(y * n + x);
            const uint32_t b = static_cast<uint32_t>(y * n + x + 1);
            const uint32_t c = static_cast<uint32_t>((y + 1) * n + x);
            const uint32_t d = static_cast<uint32_t>((y + 1) * n + x + 1);
            outIndices.push_back(a);
            outIndices.push_back(c);
            outIndices.push_back(b);
            outIndices.push_back(b);
            outIndices.push_back(c);
            outIndices.push_back(d);
        }
    }
}

// Append a downward skirt wall around the tile's 4 edges. Called after the grid
// vertices/texcoords are final (primitive.vertices are absolute ECEF here).
// Each perimeter vertex is duplicated, dropped by `skirtHeight` along the
// geodetic normal, with imagery texcoords copied so the wall drapes seamlessly.
// Edge ordering + triangle winding mirror GltfTerrainUpsampler::addSkirts
// (west N→S, south W→E, east S→N, north E→W; (topA,topB,skirtA)/(skirtA,topB,
// skirtB)) so the wall faces outward and is not back-face culled.
void appendRegularGridSkirt(GltfPrimitive& primitive,
                            int n,
                            const Rectangle& geographicRectangle) {
    if (n < 2 ||
        static_cast<int>(primitive.vertices.size()) < n * n) {
        return;
    }
    const Ellipsoid& ellipsoid = Ellipsoid::WGS84();
    const double skirtHeight =
        calcQuadtreeSkirtHeight(ellipsoid, geographicRectangle);

    SkirtMetadata meta;
    meta.noSkirtVerticesBegin = 0;
    meta.noSkirtVerticesCount =
        static_cast<uint32_t>(primitive.vertices.size());
    meta.noSkirtIndicesBegin = 0;
    meta.noSkirtIndicesCount =
        static_cast<uint32_t>(primitive.indices.size());
    meta.meshCenter = Vec3::zero();  // vertices are absolute ECEF at this stage
    meta.skirtWestHeight = skirtHeight;
    meta.skirtSouthHeight = skirtHeight;
    meta.skirtEastHeight = skirtHeight;
    meta.skirtNorthHeight = skirtHeight;

    const auto gridIndex = [n](int x, int y) {
        return static_cast<uint32_t>(y * n + x);
    };
    const auto appendEdge = [&](const std::vector<uint32_t>& edge) {
        if (edge.size() < 2) {
            return;
        }
        const uint32_t firstSkirt =
            static_cast<uint32_t>(primitive.vertices.size());
        for (uint32_t sourceIndex : edge) {
            SurfaceVertex skirtVertex = primitive.vertices[sourceIndex];
            const Vec3 top = skirtVertex.positionEcef;  // absolute ECEF
            Vec3 normal = ellipsoid.geodeticSurfaceNormal(top);
            if (normal.lengthSquared() > 0.0) {
                normal = normal.normalized();
            }
            setLocalPosition(skirtVertex, top - normal * skirtHeight);
            primitive.vertices.push_back(skirtVertex);
            // Copy the edge vertex's overlay texcoords so imagery drapes down
            // the wall (skirt shares the edge's lon/lat, only height differs).
            for (size_t set = 0; set < kGltfMaxTexCoordSets; ++set) {
                if (primitive.vertexTexCoords[set].size() + 1 ==
                    primitive.vertices.size()) {
                    primitive.vertexTexCoords[set].push_back(
                        primitive.vertexTexCoords[set][sourceIndex]);
                }
            }
        }
        for (size_t i = 0; i + 1 < edge.size(); ++i) {
            const uint32_t topA = edge[i];
            const uint32_t topB = edge[i + 1];
            const uint32_t skirtA = firstSkirt + static_cast<uint32_t>(i);
            const uint32_t skirtB = firstSkirt + static_cast<uint32_t>(i + 1);
            primitive.indices.push_back(topA);
            primitive.indices.push_back(topB);
            primitive.indices.push_back(skirtA);
            primitive.indices.push_back(skirtA);
            primitive.indices.push_back(topB);
            primitive.indices.push_back(skirtB);
        }
    };

    std::vector<uint32_t> edge;
    edge.reserve(static_cast<size_t>(n));
    edge.clear();  // west: (0,0)→(0,n-1)  north→south
    for (int y = 0; y < n; ++y) edge.push_back(gridIndex(0, y));
    appendEdge(edge);
    edge.clear();  // south: (0,n-1)→(n-1,n-1)  west→east
    for (int x = 0; x < n; ++x) edge.push_back(gridIndex(x, n - 1));
    appendEdge(edge);
    edge.clear();  // east: (n-1,n-1)→(n-1,0)  south→north
    for (int y = n - 1; y >= 0; --y) edge.push_back(gridIndex(n - 1, y));
    appendEdge(edge);
    edge.clear();  // north: (n-1,0)→(0,0)  east→west
    for (int x = n - 1; x >= 0; --x) edge.push_back(gridIndex(x, 0));
    appendEdge(edge);

    primitive.skirtMetadata = meta;
}

} // namespace

RasterOverlayDetails EllipsoidTerrainMeshBuilder::makeRasterOverlayDetails(
    const Rectangle& geographicRectangle,
    RasterOverlayProjection projection) {
    return makeRasterOverlayDetails(
        geographicRectangle,
        std::vector<RasterOverlayProjection>{projection});
}

RasterOverlayDetails EllipsoidTerrainMeshBuilder::makeRasterOverlayDetails(
    const Rectangle& geographicRectangle,
    const std::vector<RasterOverlayProjection>& projections) {
    RasterOverlayDetails details;
    details.rasterOverlayProjections = projections;
    details.rasterOverlayRectangles.reserve(projections.size());
    details.rasterOverlayInvertedVCoordinates.assign(
        projections.size(),
        false);
    for (RasterOverlayProjection projection : projections) {
        details.rasterOverlayRectangles.push_back(
            projectRasterRectangle(geographicRectangle, projection));
    }
    details.boundingRegion = {
        geographicRectangle,
        kEllipsoidTerrainMinimumHeight,
        kEllipsoidTerrainMaximumHeight};
    return details;
}

std::unique_ptr<GltfModel> EllipsoidTerrainMeshBuilder::makeModel(
    const Rectangle& geographicRectangle,
    RasterOverlayProjection projection,
    int gridSize,
    const EllipsoidProxyHeightSampler& heightSampler,
    bool computeGridNormals,
    bool computeGeomorphDelta,
    bool buildSkirt) {
    return makeEllipsoidModel(
        geographicRectangle,
        std::vector<RasterOverlayProjection>{projection},
        gridSize,
        heightSampler,
        computeGridNormals,
        computeGeomorphDelta,
        buildSkirt);
}

std::unique_ptr<GltfModel> EllipsoidTerrainMeshBuilder::makeModel(
    const Rectangle& geographicRectangle,
    const std::vector<RasterOverlayProjection>& projections,
    int gridSize,
    const EllipsoidProxyHeightSampler& heightSampler,
    bool computeGridNormals,
    bool computeGeomorphDelta,
    bool buildSkirt) {
    return makeEllipsoidModel(
        geographicRectangle, projections, gridSize, heightSampler,
        computeGridNormals, computeGeomorphDelta, buildSkirt);
}

std::unique_ptr<GltfModel> makeEllipsoidModel(
    const Rectangle& geographicRectangle,
    const std::vector<RasterOverlayProjection>& projections,
    int gridSize,
    const EllipsoidProxyHeightSampler& heightSampler,
    bool computeGridNormals,
    bool computeGeomorphDelta,
    bool buildSkirt) {
    std::vector<SurfaceVertex> gridVertices;
    std::vector<uint32_t> gridIndices;
    buildEllipsoidGrid(
        geographicRectangle,
        gridSize,
        heightSampler,
        computeGridNormals,
        computeGeomorphDelta,
        gridVertices,
        gridIndices);
    auto model = std::make_unique<GltfModel>();
    const Cartographic center = Cartographic::fromRadians(
        longitudeAt(geographicRectangle, 0.5),
        (geographicRectangle.south() + geographicRectangle.north()) * 0.5,
        0.0);
    const Vec3 localOrigin = Ellipsoid::WGS84().cartographicToCartesian(center);
    model->preferredLocalOriginEcef = localOrigin;
    model->rasterOverlayDetails =
        EllipsoidTerrainMeshBuilder::makeRasterOverlayDetails(
            geographicRectangle, projections);

    GltfNodeRuntime rootNode;
    rootNode.baseLocalTransform = Mat4::translation(localOrigin);
    rootNode.localTransform = rootNode.baseLocalTransform;
    rootNode.globalTransform = rootNode.baseLocalTransform;
    rootNode.baseTranslation = {
        localOrigin.x(),
        localOrigin.y(),
        localOrigin.z()};
    rootNode.translation = rootNode.baseTranslation;
    rootNode.mesh = 0;
    rootNode.hasMatrix = false;
    model->nodes.push_back(rootNode);
    model->sceneRootNodes.push_back(0);

    GltfPrimitive primitive;
    primitive.vertices = std::move(gridVertices);
    primitive.indices = std::move(gridIndices);
    primitive.primitiveMode = GltfPrimitiveMode::Triangles;
    primitive.doubleSided = false;
    primitive.metallicFactor = 0.0f;
    primitive.roughnessFactor = 1.0f;
    primitive.runtime.nodeIndex = 0;
    const size_t projectionCount =
        std::min(projections.size(), kGltfMaxTexCoordSets);
    for (size_t i = 0; i < projectionCount; ++i) {
        rewriteProjectionTexCoords(
            primitive,
            projections[i],
            geographicRectangle,
            i);
    }
    // Skirts are appended after texcoords so the wall's draped imagery copies
    // the final edge texcoords, and before the baseVertices/localOrigin split so
    // skirt vertices are localised together with the grid.
    if (buildSkirt) {
        appendRegularGridSkirt(
            primitive,
            std::max(1, gridSize) + 1,
            geographicRectangle);
    }
    primitive.runtime.baseVertices = primitive.vertices;
    for (SurfaceVertex& vertex : primitive.runtime.baseVertices) {
        setLocalPosition(vertex, vertex.positionEcef - localOrigin);
    }
    primitive.runtime.hasNormals = true;
    model->primitives.push_back(std::move(primitive));
    if (!model->rebuildRuntime()) {
        return nullptr;
    }
    return model;
}

std::unique_ptr<GltfModel> EllipsoidTerrainMeshBuilder::makeTemplateOnlyModel(
    const Rectangle& geographicRectangle,
    const std::vector<RasterOverlayProjection>& projections,
    int gridSize) {
    auto model = std::make_unique<GltfModel>();
    const Cartographic center = Cartographic::fromRadians(
        longitudeAt(geographicRectangle, 0.5),
        (geographicRectangle.south() + geographicRectangle.north()) * 0.5,
        0.0);
    const Vec3 localOrigin = Ellipsoid::WGS84().cartographicToCartesian(center);
    model->preferredLocalOriginEcef = localOrigin;
    model->rasterOverlayDetails =
        makeRasterOverlayDetails(geographicRectangle, projections);

    GltfNodeRuntime rootNode;
    rootNode.baseLocalTransform = Mat4::translation(localOrigin);
    rootNode.localTransform = rootNode.baseLocalTransform;
    rootNode.globalTransform = rootNode.baseLocalTransform;
    rootNode.baseTranslation = {
        localOrigin.x(),
        localOrigin.y(),
        localOrigin.z()};
    rootNode.translation = rootNode.baseTranslation;
    rootNode.mesh = 0;
    rootNode.hasMatrix = false;
    model->nodes.push_back(rootNode);
    model->sceneRootNodes.push_back(0);

    GltfPrimitive primitive;
    primitive.templateGeometryOnly = true;
    // 计数按「规则栅格 + 四条裙边」算，与 makeModel 造出来的那幅一致 ——
    // 这两个数只进 metadata 与命令构建，不决定真实绘制量（真实量来自共享
    // 模板的 VBO/IBO）。口径一致是为了诊断/字节统计读数不跳变。
    const int n = std::max(1, gridSize) + 1;
    primitive.templateVertexCount = n * n + 4 * n;
    primitive.templateIndexCount =
        (n - 1) * (n - 1) * 6 + 4 * (n - 1) * 6;
    // 排序中心：正常路径由 primitiveSortCenterEcef 从顶点算，这里没有顶点，
    // 用瓦片中心（模板落位帧的原点，与真实几何中心同点）。
    primitive.templateSortCenterEcef = localOrigin;
    primitive.primitiveMode = GltfPrimitiveMode::Triangles;
    primitive.doubleSided = false;
    primitive.metallicFactor = 0.0f;
    primitive.roughnessFactor = 1.0f;
    primitive.runtime.nodeIndex = 0;
    primitive.runtime.hasNormals = true;
    model->primitives.push_back(std::move(primitive));
    // rebuildRuntime 对无顶点 primitive 是 no-op（baseVertices 为空时按设计
    // 早返回），根节点变换已在上面手工建好。
    return model;
}

} // namespace earth_engine
