#include "EllipsoidTerrainMeshBuilder.h"

#include "GltfModel.h"
#include "../core/geodesy/Cartographic.h"
#include "../core/geodesy/Ellipsoid.h"
#include "../core/geodesy/Projection.h"
#include "../core/geodesy/WebMercatorProjection.h"
#include "../core/math/MathUtils.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace earth_engine {
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
    if (projection == RasterOverlayProjection::WebMercator) {
        return projectRectangleSimple(
            WebMercatorProjection(Ellipsoid::WGS84()),
            geographicRectangle);
    }
    return geographicRectangle;
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
    WebMercatorProjection webMercator(ellipsoid);
    for (const SurfaceVertex& vertex : primitive.vertices) {
        const std::optional<Cartographic> cartographic =
            ellipsoid.tryCartesianToCartographic(vertex.positionEcef);
        if (!cartographic || width <= 0.0 || height <= 0.0) {
            texCoords.push_back({0.0f, 0.0f});
            continue;
        }
        const Vec3 projected = projection == RasterOverlayProjection::WebMercator
            ? projectPosition(webMercator, *cartographic)
            : Vec3(
                  cartographic->longitude(),
                  cartographic->latitude(),
                  cartographic->height());
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
                        std::vector<SurfaceVertex>& outVertices,
                        std::vector<uint32_t>& outIndices) {
    const int safeGrid = std::max(1, gridSize);
    const int n = safeGrid + 1;
    const Ellipsoid& ellipsoid = Ellipsoid::WGS84();

    outVertices.clear();
    outIndices.clear();
    outVertices.reserve(static_cast<size_t>(n * n));
    outIndices.reserve(static_cast<size_t>(safeGrid * safeGrid * 6));

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
    bool computeGridNormals) {
    return makeModel(
        geographicRectangle,
        std::vector<RasterOverlayProjection>{projection},
        gridSize,
        heightSampler,
        computeGridNormals);
}

std::unique_ptr<GltfModel> EllipsoidTerrainMeshBuilder::makeModel(
    const Rectangle& geographicRectangle,
    const std::vector<RasterOverlayProjection>& projections,
    int gridSize,
    const EllipsoidProxyHeightSampler& heightSampler,
    bool computeGridNormals) {
    std::vector<SurfaceVertex> gridVertices;
    std::vector<uint32_t> gridIndices;
    buildEllipsoidGrid(
        geographicRectangle,
        gridSize,
        heightSampler,
        computeGridNormals,
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
    primitive.vertices = std::move(gridVertices);
    primitive.indices = std::move(gridIndices);
    primitive.primitiveMode = GltfPrimitiveMode::Triangles;
    primitive.doubleSided = false;
    primitive.metallicFactor = 0.0f;
    primitive.roughnessFactor = 1.0f;
    primitive.unlit = false;
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

} // namespace earth_engine
