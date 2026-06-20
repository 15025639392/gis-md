#include <gtest/gtest.h>

#include "earth_engine/tiling/TileSurface.h"
#include "earth_engine/tiling/TileScheme.h"
#include "earth_engine/tiling/TilePlan.h"
#include "earth_engine/core/geodesy/Ellipsoid.h"
#include "earth_engine/terrain/TerrainTile.h"
#include "earth_engine/providers/TerrainProvider.h"

#include <cmath>
#include <algorithm>
#include <limits>
#include <optional>
#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

using namespace earth_engine;

namespace {

double mixDouble(double a, double b, double t) {
    return a + (b - a) * t;
}

float latitudeToMercatorYFloat(float latRad) {
    constexpr float kMaxWebMercatorLat = 1.4844222297453324f;
    const float lat = std::clamp(latRad, -kMaxWebMercatorLat, kMaxWebMercatorLat);
    return (glm::pi<float>() -
            std::log(std::tan(lat * 0.5f + glm::quarter_pi<float>()))) /
           glm::two_pi<float>();
}

float mercatorYToLatitudeFloat(float y) {
    return std::atan(std::sinh(glm::pi<float>() - glm::two_pi<float>() * y));
}

glm::vec3 cartographicToWgs84EcefFloat(float longitude, float latitude) {
    const float cosLat = std::cos(latitude);
    glm::vec3 n(
        cosLat * std::cos(longitude),
        cosLat * std::sin(longitude),
        std::sin(latitude));
    n = glm::normalize(n);

    const glm::vec3 radiiSq(
        6378137.0f * 6378137.0f,
        6378137.0f * 6378137.0f,
        6356752.314245f * 6356752.314245f);
    glm::vec3 k = radiiSq * n;
    const float gamma = std::sqrt(glm::dot(n, k));
    return k / gamma;
}

Vec3 currentInstancedShaderRelativePositionFloatLike(const Rectangle& bounds,
                                                     double u,
                                                     double v,
                                                     const Vec3& localOrigin) {
    constexpr float kMaxWebMercatorLat = 1.4844222297453324f;
    const float west = static_cast<float>(bounds.west());
    const float south = static_cast<float>(bounds.south());
    const float east = static_cast<float>(bounds.east());
    const float north = static_cast<float>(bounds.north());
    const float fu = static_cast<float>(u);
    const float fv = static_cast<float>(v);

    const float longitude = west + (east - west) * fu;
    float latitude = 0.0f;
    if (south >= -kMaxWebMercatorLat && north <= kMaxWebMercatorLat) {
        const float northY = latitudeToMercatorYFloat(north);
        const float southY = latitudeToMercatorYFloat(south);
        latitude = mercatorYToLatitudeFloat(northY + (southY - northY) * fv);
    } else {
        latitude = north + (south - north) * fv;
    }

    const glm::vec3 world = cartographicToWgs84EcefFloat(longitude, latitude);
    const glm::vec3 origin(
        static_cast<float>(localOrigin.x()),
        static_cast<float>(localOrigin.y()),
        static_cast<float>(localOrigin.z()));
    const glm::vec3 relative = world - origin;
    return Vec3(relative.x, relative.y, relative.z);
}

Vec3 cornerPatchInstancedRelativePositionFloatLike(const Rectangle& bounds,
                                                   double u,
                                                   double v,
                                                   const Vec3& localOrigin) {
    const Vec3 nw = TileSurface::vertexForUnitUv(bounds, 0.0, 0.0).ecef - localOrigin;
    const Vec3 ne = TileSurface::vertexForUnitUv(bounds, 1.0, 0.0).ecef - localOrigin;
    const Vec3 sw = TileSurface::vertexForUnitUv(bounds, 0.0, 1.0).ecef - localOrigin;
    const Vec3 se = TileSurface::vertexForUnitUv(bounds, 1.0, 1.0).ecef - localOrigin;

    const glm::vec3 fnw(
        static_cast<float>(nw.x()),
        static_cast<float>(nw.y()),
        static_cast<float>(nw.z()));
    const glm::vec3 fne(
        static_cast<float>(ne.x()),
        static_cast<float>(ne.y()),
        static_cast<float>(ne.z()));
    const glm::vec3 fsw(
        static_cast<float>(sw.x()),
        static_cast<float>(sw.y()),
        static_cast<float>(sw.z()));
    const glm::vec3 fse(
        static_cast<float>(se.x()),
        static_cast<float>(se.y()),
        static_cast<float>(se.z()));

    const float fu = static_cast<float>(u);
    const float fv = static_cast<float>(v);
    const glm::vec3 north = fnw + (fne - fnw) * fu;
    const glm::vec3 south = fsw + (fse - fsw) * fu;
    const glm::vec3 relative = north + (south - north) * fv;
    return Vec3(relative.x, relative.y, relative.z);
}

Vec3 cpuMeshUploadRelativePositionFloatLike(const Rectangle& bounds,
                                            double u,
                                            double v,
                                            const Vec3& localOrigin) {
    const Vec3 world = TileSurface::vertexForUnitUv(bounds, u, v).ecef;
    const Vec3 relative = world - localOrigin;
    return Vec3(
        static_cast<float>(relative.x()),
        static_cast<float>(relative.y()),
        static_cast<float>(relative.z()));
}

Vec3 ellipsoidOriginForBounds(const Rectangle& bounds) {
    double east = bounds.east();
    if (bounds.crossesAntimeridian()) {
        east += glm::two_pi<double>();
    }
    double longitude = mixDouble(bounds.west(), east, 0.5);
    if (longitude > glm::pi<double>()) {
        longitude -= glm::two_pi<double>();
    }
    const double latitude = mixDouble(bounds.south(), bounds.north(), 0.5);
    return Ellipsoid::WGS84().cartographicToCartesian(
        Cartographic::fromRadians(longitude, latitude, 0.0));
}

} // namespace

TEST(TileSurfaceTest, UnitUvMapsTopLeftToNorthWestAndBottomRightToSouthEast) {
    auto scheme = TileScheme::createXYZWebMercator();
    Rectangle bounds = scheme->tileToRectangle(TileKey{"XYZ-WebMercator", 1, 1, 1});

    TileSurfaceVertex nw = TileSurface::vertexForUnitUv(bounds, 0.0, 0.0);
    TileSurfaceVertex se = TileSurface::vertexForUnitUv(bounds, 1.0, 1.0);

    Cartographic nwCart = Ellipsoid::WGS84().cartesianToCartographic(nw.ecef);
    Cartographic seCart = Ellipsoid::WGS84().cartesianToCartographic(se.ecef);

    EXPECT_NEAR(bounds.west(), nwCart.longitude(), 1e-9);
    EXPECT_NEAR(bounds.north(), nwCart.latitude(), 1e-9);
    EXPECT_NEAR(bounds.east(), seCart.longitude(), 1e-9);
    EXPECT_NEAR(bounds.south(), seCart.latitude(), 1e-9);
    EXPECT_FLOAT_EQ(0.0f, nw.uv[0]);
    EXPECT_FLOAT_EQ(0.0f, nw.uv[1]);
    EXPECT_FLOAT_EQ(1.0f, se.uv[0]);
    EXPECT_FLOAT_EQ(1.0f, se.uv[1]);
}

TEST(TileSurfaceTest, VerticesLieOnWgs84EllipsoidSurface) {
    auto scheme = TileScheme::createXYZWebMercator();
    Rectangle bounds = scheme->tileToRectangle(TileKey{"XYZ-WebMercator", 3, 4, 3});

    for (double v : {0.0, 0.25, 0.5, 0.75, 1.0}) {
        for (double u : {0.0, 0.25, 0.5, 0.75, 1.0}) {
            TileSurfaceVertex vertex = TileSurface::vertexForUnitUv(bounds, u, v);
            Cartographic cart = Ellipsoid::WGS84().cartesianToCartographic(vertex.ecef);
            EXPECT_NEAR(0.0, cart.height(), 1e-4);
        }
    }
}

TEST(TileSurfaceTest, TileTrianglesFaceOutwardForBackfaceCulling) {
    auto scheme = TileScheme::createXYZWebMercator();

    EXPECT_TRUE(TileSurface::trianglesFaceOutward(
        scheme->tileToRectangle(TileKey{"XYZ-WebMercator", 2, 2, 1})));
    EXPECT_TRUE(TileSurface::trianglesFaceOutward(
        scheme->tileToRectangle(TileKey{"XYZ-WebMercator", 4, 8, 8})));
}

TEST(TileSurfaceTest, EllipsoidMeshHasExpectedGridAndOutwardTriangles) {
    auto scheme = TileScheme::createXYZWebMercator();
    Rectangle bounds = scheme->tileToRectangle(TileKey{"XYZ-WebMercator", 3, 4, 3});

    SurfaceTileMesh mesh = TileSurface::buildEllipsoidMesh(bounds, 4);

    EXPECT_EQ(4, mesh.gridSize);
    EXPECT_EQ(SurfaceTileMeshWinding::Outward, mesh.winding);
    EXPECT_EQ(SurfaceTileSampling::GeographicVToWgs84Ecef, mesh.sampling);
    EXPECT_EQ(25u, mesh.vertices.size());
    EXPECT_EQ(96u, mesh.indices.size());

    for (const SurfaceVertex& vertex : mesh.vertices) {
        EXPECT_NEAR(1.0, vertex.normalEcef.length(), 1e-9);
        Vec3 reconstructed = vertex.positionHighEcef + vertex.positionLowEcef;
        EXPECT_NEAR(0.0, reconstructed.distanceTo(vertex.positionEcef), 1e-9);
        EXPECT_LT(std::abs(vertex.positionLowEcef.x()), 65536.0);
        EXPECT_LT(std::abs(vertex.positionLowEcef.y()), 65536.0);
        EXPECT_LT(std::abs(vertex.positionLowEcef.z()), 65536.0);
        EXPECT_GE(vertex.uv[0], 0.0f);
        EXPECT_LE(vertex.uv[0], 1.0f);
        EXPECT_GE(vertex.uv[1], 0.0f);
        EXPECT_LE(vertex.uv[1], 1.0f);
    }

    for (size_t i = 0; i < mesh.indices.size(); i += 3) {
        const Vec3& a = mesh.vertices[mesh.indices[i]].positionEcef;
        const Vec3& b = mesh.vertices[mesh.indices[i + 1]].positionEcef;
        const Vec3& c = mesh.vertices[mesh.indices[i + 2]].positionEcef;
        const Vec3 n = (b - a).cross(c - a);
        const Vec3 center = (a + b + c) / 3.0;
        EXPECT_GT(n.dot(center), 0.0);
    }
}

TEST(TileSurfaceTest, CurrentInstancedShaderFloatEcefPathLosesNearGroundPrecision) {
    auto scheme = TileScheme::createXYZWebMercator();
    Rectangle bounds = scheme->tileToRectangle(TileKey{"XYZ-WebMercator", 18, 212000, 107000});
    Vec3 localOrigin = ellipsoidOriginForBounds(bounds);

    double maxErrorMeters = 0.0;
    for (double v : {0.0, 0.125, 0.25, 0.5, 0.75, 0.875, 1.0}) {
        for (double u : {0.0, 0.125, 0.25, 0.5, 0.75, 0.875, 1.0}) {
            const Vec3 cpuUploaded =
                cpuMeshUploadRelativePositionFloatLike(bounds, u, v, localOrigin);
            const Vec3 shaderLike =
                currentInstancedShaderRelativePositionFloatLike(bounds, u, v, localOrigin);
            maxErrorMeters = std::max(maxErrorMeters, cpuUploaded.distanceTo(shaderLike));
        }
    }

    // The old path subtracts the tile origin in double before uploading small
    // float positions. The current GLES instanced path computes two ECEF-sized
    // float values in shader and subtracts them there, which is not reliable at
    // near-ground camera heights.
    EXPECT_GT(maxErrorMeters, 1.0);
}

TEST(TileSurfaceTest, TileLocalCornerPatchKeepsHighZoomNearGroundPrecision) {
    auto scheme = TileScheme::createXYZWebMercator();
    Rectangle bounds = scheme->tileToRectangle(TileKey{"XYZ-WebMercator", 18, 212000, 107000});
    Vec3 localOrigin = ellipsoidOriginForBounds(bounds);

    double maxErrorMeters = 0.0;
    for (double v : {0.0, 0.125, 0.25, 0.5, 0.75, 0.875, 1.0}) {
        for (double u : {0.0, 0.125, 0.25, 0.5, 0.75, 0.875, 1.0}) {
            const Vec3 cpuUploaded =
                cpuMeshUploadRelativePositionFloatLike(bounds, u, v, localOrigin);
            const Vec3 patch =
                cornerPatchInstancedRelativePositionFloatLike(bounds, u, v, localOrigin);
            maxErrorMeters = std::max(maxErrorMeters, cpuUploaded.distanceTo(patch));
        }
    }

    // At high zoom the tile is small enough that a tile-local patch avoids
    // ECEF-sized float subtraction while staying below near-ground visual error.
    EXPECT_LT(maxErrorMeters, 0.25);
}

TEST(TileSurfaceTest, EllipsoidMeshClampsMinimumGridSize) {
    auto scheme = TileScheme::createXYZWebMercator();
    Rectangle bounds = scheme->tileToRectangle(TileKey{"XYZ-WebMercator", 1, 1, 1});

    SurfaceTileMesh mesh = TileSurface::buildEllipsoidMesh(bounds, 0);

    EXPECT_EQ(1, mesh.gridSize);
    EXPECT_EQ(4u, mesh.vertices.size());
    EXPECT_EQ(6u, mesh.indices.size());
}

TEST(TileSurfaceTest, ParentFallbackUvWindowSelectsChildQuadrant) {
    auto scheme = TileScheme::createGeographicTMS();

    TileKey child{"Geographic-TMS", 2, 2, 0};
    TileKey parent{"Geographic-TMS", 1, 1, 0};
    TileTextureWindow nativeWindow = TileSurface::computeTranslationAndScale(
        scheme->tileToRectangle(child),
        scheme->tileToRectangle(parent));
    TileTextureWindow window =
        TileSurface::textureWindowForNorthWestUv(nativeWindow);

    EXPECT_NEAR(0.0f, nativeWindow.offsetV, 1e-6f);
    EXPECT_NEAR(0.0f, window.offsetU, 1e-6f);
    EXPECT_NEAR(0.5f, window.offsetV, 1e-6f);
    EXPECT_NEAR(0.5f, window.scaleU, 1e-6f);
    EXPECT_NEAR(0.5f, window.scaleV, 1e-6f);
}

TEST(TileSurfaceTest, TextureWindowMatchesCesiumNativeTranslationAndScale) {
    // Ported from cesium-native RasterOverlayUtilities::computeTranslationAndScale:
    // translation = (geometry.min - overlay.min) / overlay.size, scale =
    // geometry.size / overlay.size.
    const Rectangle imageryBounds(10.0, 20.0, 50.0, 100.0);
    const Rectangle geometryBounds(18.0, 44.0, 38.0, 84.0);

    const TileTextureWindow nativeWindow =
        TileSurface::computeTranslationAndScale(geometryBounds, imageryBounds);

    EXPECT_NEAR(0.2f, nativeWindow.offsetU, 1e-6f);
    EXPECT_NEAR(0.3f, nativeWindow.offsetV, 1e-6f);
    EXPECT_NEAR(0.5f, nativeWindow.scaleU, 1e-6f);
    EXPECT_NEAR(0.5f, nativeWindow.scaleV, 1e-6f);

    const TileTextureWindow rendererWindow =
        TileSurface::textureWindowForNorthWestUv(nativeWindow);
    EXPECT_NEAR(0.2f, rendererWindow.offsetU, 1e-6f);
    EXPECT_NEAR(0.2f, rendererWindow.offsetV, 1e-6f);
    EXPECT_NEAR(0.5f, rendererWindow.scaleU, 1e-6f);
    EXPECT_NEAR(0.5f, rendererWindow.scaleV, 1e-6f);
}

TEST(TileSurfaceTest, OpenGlobusPolarMeshUsesGeographicVSampling) {
    auto scheme = TileScheme::createOpenGlobusEarth();
    TileKey northKey = scheme->positionToTile(0.0, 88.0 * M_PI / 180.0, 3);
    Rectangle bounds = scheme->tileToRectangle(northKey);

    SurfaceTileMesh mesh = TileSurface::buildEllipsoidMesh(bounds, 2);

    EXPECT_EQ(SurfaceTileSampling::GeographicVToWgs84Ecef, mesh.sampling);
    ASSERT_EQ(9u, mesh.vertices.size());

    Cartographic northWest = Ellipsoid::WGS84().cartesianToCartographic(
        mesh.vertices[0].positionEcef);
    Cartographic southEast = Ellipsoid::WGS84().cartesianToCartographic(
        mesh.vertices.back().positionEcef);

    EXPECT_NEAR(bounds.north(), northWest.latitude(), 1e-9);
    EXPECT_NEAR(bounds.south(), southEast.latitude(), 1e-9);
    EXPECT_GT(northWest.latitude(), 85.05 * M_PI / 180.0);
}

TEST(TileSurfaceTest, OpenGlobusPolarParentTextureWindowUsesGeographicV) {
    auto scheme = TileScheme::createOpenGlobusEarth();
    TileKey child = scheme->positionToTile(45.0 * M_PI / 180.0,
                                           88.0 * M_PI / 180.0,
                                           2);
    TileKey parent = TilePlanBuilder::parentKey(child);

    TileTextureWindow nativeWindow = TileSurface::computeTranslationAndScale(
        scheme->tileToRectangle(child),
        scheme->tileToRectangle(parent));
    TileTextureWindow window =
        TileSurface::textureWindowForNorthWestUv(nativeWindow);

    EXPECT_GE(window.offsetU, 0.0f);
    EXPECT_GE(window.offsetV, 0.0f);
    EXPECT_NEAR(0.5f, window.scaleU, 1e-6f);
    EXPECT_NEAR(0.5f, window.scaleV, 1e-6f);
}

TEST(TileSurfaceTest, SurfaceTileTerrainSamplesHeightInEcefPath) {
    auto scheme = TileScheme::createXYZWebMercator();
    TileKey key{"XYZ-WebMercator", 1, 1, 1};
    Rectangle bounds = scheme->tileToRectangle(key);

    auto heightmap = std::make_unique<DecodedHeightmap>();
    heightmap->tileSize = 2;
    heightmap->heights = {100.0f, 100.0f, 100.0f, 100.0f};
    heightmap->minHeight = 100.0f;
    heightmap->maxHeight = 100.0f;
    TerrainTile terrain(key, *scheme, std::move(heightmap));

    SurfaceTileMesh terrainMesh = TileSurface::buildTerrainMesh(bounds, &terrain, 2);
    ASSERT_EQ(9u, terrainMesh.vertices.size());

    for (const SurfaceVertex& vertex : terrainMesh.vertices) {
        Cartographic cart = Ellipsoid::WGS84().cartesianToCartographic(vertex.positionEcef);
        EXPECT_NEAR(100.0, cart.height(), 1e-3);
        EXPECT_NEAR(1.0, vertex.normalEcef.length(), 1e-9);
    }
}

TEST(TileSurfaceTest, SurfaceTileTerrainSamplesParentTileByCartographicCrop) {
    auto scheme = TileScheme::createXYZWebMercator();
    TileKey parentKey{"XYZ-WebMercator", 1, 1, 0};
    TileKey childKey{"XYZ-WebMercator", 2, 2, 1};
    Rectangle childBounds = scheme->tileToRectangle(childKey);

    auto heightmap = std::make_unique<DecodedHeightmap>();
    heightmap->tileSize = 2;
    heightmap->heights = {0.0f, 100.0f, 200.0f, 300.0f};
    heightmap->minHeight = 0.0f;
    heightmap->maxHeight = 300.0f;
    TerrainTile parentTerrain(parentKey, *scheme, std::move(heightmap));

    SurfaceTileMesh childMesh = TileSurface::buildTerrainMesh(
        childBounds, &parentTerrain, 2);
    ASSERT_EQ(9u, childMesh.vertices.size());

    const SurfaceVertex& centerVertex = childMesh.vertices[4];
    Cartographic center = Ellipsoid::WGS84().cartesianToCartographic(
        centerVertex.positionEcef);
    const double expected = parentTerrain.sampleHeight(
        center.longitude(), center.latitude());

    EXPECT_NEAR(expected, center.height(), 1e-3);
    EXPECT_GT(expected, 0.0);
    EXPECT_LT(expected, 300.0);
}

TEST(TileSurfaceTest, UpsampledChildMeshIsClippedFromParentRenderMesh) {
    Rectangle parentBounds = Rectangle::fromDegrees(0.0, 0.0, 2.0, 2.0);
    Rectangle childBounds = Rectangle::fromDegrees(1.0, 1.0, 2.0, 2.0);

    SurfaceTileMesh parentMesh = TileSurface::buildEllipsoidMesh(parentBounds, 1);
    ASSERT_EQ(4u, parentMesh.vertices.size());

    const auto& ellipsoid = Ellipsoid::WGS84();
    Cartographic raised = ellipsoid.cartesianToCartographic(
        parentMesh.vertices[1].positionEcef);
    parentMesh.vertices[1].positionEcef = ellipsoid.cartographicToCartesian(
        Cartographic::fromRadians(
            raised.longitude(),
            raised.latitude(),
            1000.0));

    std::optional<SurfaceTileMesh> childMesh =
        TileSurface::upsampleChildMeshFromParent(
            parentMesh,
            parentBounds,
            childBounds);

    ASSERT_TRUE(childMesh.has_value());
    EXPECT_GT(childMesh->vertices.size(), 0u);
    EXPECT_GT(childMesh->indices.size(), 0u);
    EXPECT_TRUE(childMesh->hasHeightRange);
    EXPECT_GT(childMesh->maximumHeight, 900.0);

    for (const SurfaceVertex& vertex : childMesh->vertices) {
        EXPECT_GE(vertex.uv[0], 0.0f);
        EXPECT_LE(vertex.uv[0], 1.0f);
        EXPECT_GE(vertex.uv[1], 0.0f);
        EXPECT_LE(vertex.uv[1], 1.0f);
    }
}

TEST(TileSurfaceTest, UpsampledChildMeshIgnoresInvalidNoSkirtIndexRange) {
    Rectangle parentBounds = Rectangle::fromDegrees(0.0, 0.0, 2.0, 2.0);
    Rectangle childBounds = Rectangle::fromDegrees(1.0, 1.0, 2.0, 2.0);

    SurfaceTileMesh parentMesh = TileSurface::buildEllipsoidMesh(parentBounds, 1);
    ASSERT_GT(parentMesh.indices.size(), 0u);

    parentMesh.skirtMeta.noSkirtIndicesBegin =
        std::numeric_limits<uint32_t>::max();
    parentMesh.skirtMeta.noSkirtIndicesCount = 2;

    std::optional<SurfaceTileMesh> childMesh =
        TileSurface::upsampleChildMeshFromParent(
            parentMesh,
            parentBounds,
            childBounds);

    ASSERT_TRUE(childMesh.has_value());
    EXPECT_GT(childMesh->vertices.size(), 0u);
    EXPECT_GT(childMesh->indices.size(), 0u);
}

TEST(TileSurfaceTest, SurfaceTileTerrainCanAddSkirt) {
    auto scheme = TileScheme::createXYZWebMercator();
    TileKey key{"XYZ-WebMercator", 1, 1, 1};
    Rectangle bounds = scheme->tileToRectangle(key);

    auto heightmap = std::make_unique<DecodedHeightmap>();
    heightmap->tileSize = 2;
    heightmap->heights = {100.0f, 100.0f, 100.0f, 100.0f};
    heightmap->minHeight = 100.0f;
    heightmap->maxHeight = 100.0f;
    TerrainTile terrain(key, *scheme, std::move(heightmap));

    SurfaceTileMesh noSkirt = TileSurface::buildTerrainMesh(bounds, &terrain, 2);
    SurfaceTileMesh withSkirt = TileSurface::buildTerrainMesh(bounds, &terrain, 2, -50.0);

    EXPECT_GT(withSkirt.vertices.size(), noSkirt.vertices.size());
    EXPECT_GT(withSkirt.indices.size(), noSkirt.indices.size());

    bool sawLowerSkirtVertex = false;
    for (size_t i = noSkirt.vertices.size(); i < withSkirt.vertices.size(); ++i) {
        Cartographic cart = Ellipsoid::WGS84().cartesianToCartographic(
            withSkirt.vertices[i].positionEcef);
        sawLowerSkirtVertex = sawLowerSkirtVertex || cart.height() < 75.0;
    }
    EXPECT_TRUE(sawLowerSkirtVertex);

    SurfaceNormalMap normalMap = TileSurface::buildNormalMap(withSkirt);
    EXPECT_TRUE(normalMap.valid());
    EXPECT_EQ(noSkirt.gridSize + 1, normalMap.width);
    EXPECT_EQ(noSkirt.gridSize + 1, normalMap.height);
}

TEST(TileSurfaceTest, SurfaceTileTerrainNormalsComeFromGeometry) {
    auto scheme = TileScheme::createXYZWebMercator();
    TileKey key{"XYZ-WebMercator", 1, 1, 1};
    Rectangle bounds = scheme->tileToRectangle(key);

    auto heightmap = std::make_unique<DecodedHeightmap>();
    heightmap->tileSize = 2;
    heightmap->heights = {0.0f, 1000.0f, 0.0f, 1000.0f};
    heightmap->minHeight = 0.0f;
    heightmap->maxHeight = 1000.0f;
    TerrainTile terrain(key, *scheme, std::move(heightmap));

    SurfaceTileMesh mesh = TileSurface::buildTerrainMesh(bounds, &terrain, 4);
    ASSERT_GT(mesh.vertices.size(), 0u);

    bool sawNonEllipsoidNormal = false;
    for (const SurfaceVertex& vertex : mesh.vertices) {
        Vec3 ellipsoidNormal = Ellipsoid::WGS84().geodeticSurfaceNormal(vertex.positionEcef);
        EXPECT_NEAR(1.0, vertex.normalEcef.length(), 1e-9);
        sawNonEllipsoidNormal = sawNonEllipsoidNormal ||
            vertex.normalEcef.dot(ellipsoidNormal) < 0.999999;
    }
    EXPECT_TRUE(sawNonEllipsoidNormal);
}

TEST(TileSurfaceTest, NormalMapEncodesSurfaceNormalsAsRgba8) {
    auto scheme = TileScheme::createXYZWebMercator();
    Rectangle bounds = scheme->tileToRectangle(TileKey{"XYZ-WebMercator", 2, 2, 1});
    SurfaceTileMesh mesh = TileSurface::buildEllipsoidMesh(bounds, 4);

    SurfaceNormalMap normalMap = TileSurface::buildNormalMap(mesh);

    ASSERT_TRUE(normalMap.valid());
    EXPECT_EQ(5, normalMap.width);
    EXPECT_EQ(5, normalMap.height);
    ASSERT_EQ(100u, normalMap.rgba.size());

    const SurfaceVertex& vertex = mesh.vertices[12];
    const size_t pixel = 12u * 4u;
    Vec3 decoded(
        static_cast<double>(normalMap.rgba[pixel]) / 255.0 * 2.0 - 1.0,
        static_cast<double>(normalMap.rgba[pixel + 1]) / 255.0 * 2.0 - 1.0,
        static_cast<double>(normalMap.rgba[pixel + 2]) / 255.0 * 2.0 - 1.0);
    decoded = decoded.normalized();

    EXPECT_GT(decoded.dot(vertex.normalEcef.normalized()), 0.999);
    EXPECT_EQ(255, normalMap.rgba[pixel + 3]);
}

TEST(TileSurfaceTest, NormalMapUsesNoSkirtVertexRangeBegin) {
    auto scheme = TileScheme::createXYZWebMercator();
    Rectangle bounds = scheme->tileToRectangle(TileKey{"XYZ-WebMercator", 2, 2, 1});
    SurfaceTileMesh surface = TileSurface::buildEllipsoidMesh(bounds, 1);
    ASSERT_EQ(4u, surface.vertices.size());

    SurfaceTileMesh mesh = surface;
    SurfaceVertex skirtLikeVertex = surface.vertices.front();
    skirtLikeVertex.normalEcef = Vec3(-1.0, 0.0, 0.0);
    mesh.vertices.insert(mesh.vertices.begin(), skirtLikeVertex);
    mesh.skirtMeta.noSkirtVerticesBegin = 1;
    mesh.skirtMeta.noSkirtVerticesCount = 4;

    SurfaceNormalMap normalMap = TileSurface::buildNormalMap(mesh);

    ASSERT_TRUE(normalMap.valid());
    ASSERT_EQ(16u, normalMap.rgba.size());
    Vec3 decoded(
        static_cast<double>(normalMap.rgba[0]) / 255.0 * 2.0 - 1.0,
        static_cast<double>(normalMap.rgba[1]) / 255.0 * 2.0 - 1.0,
        static_cast<double>(normalMap.rgba[2]) / 255.0 * 2.0 - 1.0);
    decoded = decoded.normalized();

    EXPECT_GT(decoded.dot(surface.vertices.front().normalEcef.normalized()), 0.999);
}
