#include <gtest/gtest.h>

#include "earth_engine/tiling/TileSurface.h"
#include "earth_engine/tiling/TileScheme.h"
#include "earth_engine/core/geodesy/Ellipsoid.h"

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
