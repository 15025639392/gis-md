#include <gtest/gtest.h>

#include "earth_engine/tiling/TileSurface.h"
#include "earth_engine/tiling/TileScheme.h"
#include "earth_engine/tiling/TilePlan.h"
#include "earth_engine/core/geodesy/Ellipsoid.h"
#include "earth_engine/terrain/TerrainTile.h"
#include "earth_engine/providers/TerrainProvider.h"

#include <cmath>

using namespace earth_engine;

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
    EXPECT_EQ(SurfaceTileSampling::WebMercatorVToWgs84Ecef, mesh.sampling);
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

TEST(TileSurfaceTest, EllipsoidMeshClampsMinimumGridSize) {
    auto scheme = TileScheme::createXYZWebMercator();
    Rectangle bounds = scheme->tileToRectangle(TileKey{"XYZ-WebMercator", 1, 1, 1});

    SurfaceTileMesh mesh = TileSurface::buildEllipsoidMesh(bounds, 0);

    EXPECT_EQ(1, mesh.gridSize);
    EXPECT_EQ(4u, mesh.vertices.size());
    EXPECT_EQ(6u, mesh.indices.size());
}

TEST(TileSurfaceTest, ParentFallbackUvWindowSelectsChildQuadrant) {
    auto scheme = TileScheme::createXYZWebMercator();

    TileKey child{"XYZ-WebMercator", 2, 2, 1};
    TileKey parent{"XYZ-WebMercator", 1, 1, 0};
    TileTextureWindow window = TileSurface::textureWindow(
        scheme->tileToRectangle(child),
        scheme->tileToRectangle(parent));

    EXPECT_NEAR(0.0f, window.offsetU, 1e-6f);
    EXPECT_NEAR(0.5f, window.offsetV, 1e-6f);
    EXPECT_NEAR(0.5f, window.scaleU, 1e-6f);
    EXPECT_NEAR(0.5f, window.scaleV, 1e-6f);
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

    TileTextureWindow window = TileSurface::textureWindow(
        scheme->tileToRectangle(child),
        scheme->tileToRectangle(parent));

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
