#include <gtest/gtest.h>

#include "earth_engine/content/EllipsoidTerrainMeshBuilder.h"
#include "earth_engine/content/GltfModel.h"
#include "earth_engine/core/geodesy/Cartographic.h"
#include "earth_engine/core/geodesy/Ellipsoid.h"
#include "earth_engine/core/math/Rectangle.h"
#include "earth_engine/tiling/RasterMappedToTilesetTile.h"
#include "earth_engine/tiling/TileFillProxyPreparer.h"
#include "earth_engine/tiling/TilesetTile.h"
#include "../../helpers/MockRenderDevice.h"

#include <climits>
#include <cmath>
#include <limits>
using namespace earth_engine;

namespace {

// A tile rectangle well away from poles/antimeridian so projection is smooth.
Rectangle testRectangle() {
    return Rectangle::fromDegrees(-10.0, 20.0, -2.0, 28.0);
}

} // namespace

TEST(EllipsoidTerrainMeshBuilderTest, ProducesDrapeReadyGridModel) {
    const int gridSize = 4;
    auto model = EllipsoidTerrainMeshBuilder::makeModel(
        testRectangle(),
        RasterOverlayProjection::Geographic,
        gridSize);

    ASSERT_NE(nullptr, model);
    ASSERT_EQ(1u, model->primitives.size());
    const GltfPrimitive& prim = model->primitives.front();

    const int n = gridSize + 1;
    EXPECT_EQ(static_cast<size_t>(n * n), prim.vertices.size());
    EXPECT_EQ(static_cast<size_t>(gridSize * gridSize * 6), prim.indices.size());

    // Overlay details describe the tile rectangle for a single projection with
    // the NW-V convention — exactly what the raster binding path expects.
    ASSERT_EQ(1u, model->rasterOverlayDetails.rasterOverlayProjections.size());
    EXPECT_EQ(RasterOverlayProjection::Geographic,
              model->rasterOverlayDetails.rasterOverlayProjections[0]);
    ASSERT_EQ(1u, model->rasterOverlayDetails.rasterOverlayRectangles.size());
    ASSERT_EQ(1u,
              model->rasterOverlayDetails.rasterOverlayInvertedVCoordinates
                  .size());
    EXPECT_FALSE(
        model->rasterOverlayDetails.rasterOverlayInvertedVCoordinates[0]);

    // Overlay texcoords are populated for set 0 (the single projection).
    ASSERT_EQ(prim.vertices.size(), prim.vertexTexCoords[0].size());
}

TEST(EllipsoidTerrainMeshBuilderTest,
     GeographicCornerTexcoordsSpanRectangleExactly) {
    // For Geographic projection the overlay UV is a linear function of lon/lat,
    // so the grid corners map to rectangle corners (0,0)/(1,0)/(0,1)/(1,1) with
    // the NW-V convention (v=0 at north). This is the drape-alignment property:
    // a real-terrain vertex at the same lon/lat produces the same UV, so
    // imagery stays put when the proxy is later swapped for real terrain.
    const int gridSize = 4;
    const int n = gridSize + 1;
    auto model = EllipsoidTerrainMeshBuilder::makeModel(
        testRectangle(),
        RasterOverlayProjection::Geographic,
        gridSize);
    ASSERT_NE(nullptr, model);
    const auto& uv = model->primitives.front().vertexTexCoords[0];
    ASSERT_EQ(static_cast<size_t>(n * n), uv.size());

    const size_t nw = 0;                         // (u=0, v=0) west/north
    const size_t ne = static_cast<size_t>(n - 1);
    const size_t sw = static_cast<size_t>((n - 1) * n);
    const size_t se = static_cast<size_t>(n * n - 1);

    EXPECT_NEAR(0.0f, uv[nw][0], 1e-5f);
    EXPECT_NEAR(0.0f, uv[nw][1], 1e-5f);
    EXPECT_NEAR(1.0f, uv[ne][0], 1e-5f);
    EXPECT_NEAR(0.0f, uv[ne][1], 1e-5f);
    EXPECT_NEAR(0.0f, uv[sw][0], 1e-5f);
    EXPECT_NEAR(1.0f, uv[sw][1], 1e-5f);
    EXPECT_NEAR(1.0f, uv[se][0], 1e-5f);
    EXPECT_NEAR(1.0f, uv[se][1], 1e-5f);
}

TEST(EllipsoidTerrainMeshBuilderTest,
     AntimeridianGridInterpolatesAcrossDateline) {
    const Rectangle bounds = Rectangle::fromDegrees(
        170.0,
        -10.0,
        -170.0,
        10.0);
    auto model = EllipsoidTerrainMeshBuilder::makeModel(
        bounds,
        RasterOverlayProjection::Geographic,
        2);
    ASSERT_NE(nullptr, model);
    const GltfPrimitive& primitive = model->primitives.front();
    ASSERT_EQ(9u, primitive.vertices.size());
    ASSERT_EQ(9u, primitive.vertexTexCoords[0].size());

    const std::optional<Cartographic> center =
        Ellipsoid::WGS84().tryCartesianToCartographic(
            primitive.vertices[4].positionEcef);
    ASSERT_TRUE(center.has_value());
    EXPECT_GT(std::abs(center->longitude()), 3.0);
    EXPECT_NEAR(0.5f, primitive.vertexTexCoords[0][4][0], 1e-5f);
}

TEST(EllipsoidTerrainMeshBuilderTest, AnchoredOnEllipsoidSurfaceAtHeightZero) {
    // The proxy is the "smooth globe" imagery drapes onto before real terrain
    // (with elevation) rises in, so it must sit on the WGS84 ellipsoid. The
    // model's local origin is the rectangle center placed at height 0, and the
    // overlay bounding region records the [0, 0] height range — both mark the
    // proxy as flush with the ellipsoid surface.
    const int gridSize = 4;
    auto model = EllipsoidTerrainMeshBuilder::makeModel(
        testRectangle(),
        RasterOverlayProjection::Geographic,
        gridSize);
    ASSERT_NE(nullptr, model);
    ASSERT_TRUE(model->preferredLocalOriginEcef.has_value());

    const auto originCarto = Ellipsoid::WGS84().tryCartesianToCartographic(
        *model->preferredLocalOriginEcef);
    ASSERT_TRUE(originCarto.has_value());
    EXPECT_NEAR(0.0, originCarto->height(), 1e-3);

    const auto& region = model->rasterOverlayDetails.boundingRegion;
    EXPECT_NEAR(0.0, region.minimumHeight, 1e-9);
    EXPECT_NEAR(0.0, region.maximumHeight, 1e-9);
}

TEST(EllipsoidTerrainMeshBuilderTest,
     RuntimeVerticesAreWorldEcefAndBaseVerticesAreLocalRtc) {
    const int gridSize = 4;
    auto model = EllipsoidTerrainMeshBuilder::makeModel(
        testRectangle(),
        RasterOverlayProjection::Geographic,
        gridSize);
    ASSERT_NE(nullptr, model);
    ASSERT_TRUE(model->preferredLocalOriginEcef.has_value());
    ASSERT_EQ(1u, model->nodes.size());
    ASSERT_EQ(1u, model->primitives.size());

    const Vec3& origin = *model->preferredLocalOriginEcef;
    const GltfNodeRuntime& node = model->nodes.front();
    const GltfPrimitive& prim = model->primitives.front();
    ASSERT_FALSE(prim.vertices.empty());
    ASSERT_EQ(prim.vertices.size(), prim.runtime.baseVertices.size());

    const Vec3 worldFromNode =
        node.globalTransform * prim.runtime.baseVertices.front().positionEcef;
    EXPECT_LT((prim.vertices.front().positionEcef - worldFromNode).length(),
              1e-6);
    EXPECT_LT((prim.runtime.baseVertices.front().positionEcef -
               (prim.vertices.front().positionEcef - origin))
                  .length(),
              1e-6);

    // Guard against the fill proxy double-applying the tile origin: world ECEF
    // vertices are around Earth radius, while RTC base vertices stay tile-sized.
    EXPECT_GT(prim.vertices.front().positionEcef.length(), 6.0e6);
    EXPECT_LT(prim.runtime.baseVertices.front().positionEcef.length(), 8.0e5);
}

TEST(EllipsoidTerrainMeshBuilderTest,
     FillProxyDrawsUntilRealTerrainResourcesAreReady) {
    const Rectangle bounds = testRectangle();
    TilesetTile tile(
        TileKey{"Geographic-TMS", 2, 1, 1},
        bounds);
    earth_engine::testing::MockRenderDevice device;

    ASSERT_TRUE(TileFillProxyPreparer::ensureFillProxy(
        tile,
        &device,
        4));
    const TileRenderContentState& fillState =
        tile.content.renderContent;
    EXPECT_FALSE(fillState.hasFillModel());
    ASSERT_TRUE(fillState.isFillReady());
    ASSERT_TRUE(fillState.hasFillResources());
    EXPECT_TRUE(fillState.drawsFill());
    ASSERT_EQ(1u, fillState.drawPrimitiveResources().size());
    EXPECT_TRUE(
        fillState.drawPrimitiveResources().front().useTerrainVertexFormat);
    EXPECT_EQ(2, device.createdBufferCount);
    EXPECT_FALSE(TileFillProxyPreparer::ensureFillProxy(
        tile,
        &device,
        4));
    EXPECT_EQ(2, device.createdBufferCount);

    auto realTerrain = EllipsoidTerrainMeshBuilder::makeModel(
        bounds,
        RasterOverlayProjection::Geographic,
        4);
    ASSERT_NE(nullptr, realTerrain);
    TileRenderContentState& realState =
        tile.content.renderContent;
    tile.rasterOverlayState.ensureMapping(0);
    realState.prepareGltfContent(std::move(realTerrain), Mat4::identity());
    realState.setTerrainRenderContent(true);

    GltfPrimitiveRenderResources realResources;
    BufferDesc vertexBufferDesc;
    vertexBufferDesc.size = 32;
    vertexBufferDesc.type = BufferDesc::Type::Vertex;
    realResources.vertexBuffer = device.createBuffer(vertexBufferDesc);
    BufferDesc indexBufferDesc;
    indexBufferDesc.size = 12;
    indexBufferDesc.type = BufferDesc::Type::Index;
    realResources.indexBuffer = device.createBuffer(indexBufferDesc);
    realResources.vertexCount = 3;
    realResources.indexCount = 3;
    realState.addGltfPrimitiveResource(std::move(realResources));
    realState.markRenderContentReady();

    EXPECT_TRUE(realState.isGltfRenderReady());
    EXPECT_FALSE(realState.hasFillModel());
    EXPECT_FALSE(realState.drawsFill());
    EXPECT_EQ(1u, tile.rasterOverlayState.mappingCount());
    EXPECT_FALSE(TileFillProxyPreparer::ensureFillProxy(
        tile,
        &device,
        4));
    EXPECT_EQ(1u, tile.rasterOverlayState.mappingCount());
    ASSERT_EQ(1u, realState.drawPrimitiveResources().size());
    EXPECT_FALSE(
        realState.drawPrimitiveResources().front().useTerrainVertexFormat);
}

TEST(EllipsoidTerrainMeshBuilderTest,
     FillProxyRebuildsOnlyWhenGeometrySignatureChanges) {
    TilesetTile tile(
        TileKey{"Geographic-TMS", 2, 1, 1},
        testRectangle());
    earth_engine::testing::MockRenderDevice device;

    ASSERT_TRUE(TileFillProxyPreparer::ensureFillProxy(
        tile,
        &device,
        4));
    ASSERT_EQ(2, device.createdBufferCount);

    tile.rasterOverlayState.ensureMapping(0);
    tile.bounds = Rectangle::fromDegrees(-9.0, 20.0, -1.0, 28.0);
    EXPECT_TRUE(TileFillProxyPreparer::ensureFillProxy(
        tile,
        &device,
        4));
    EXPECT_EQ(0u, tile.rasterOverlayState.mappingCount());
    EXPECT_EQ(4, device.createdBufferCount);

    EXPECT_FALSE(TileFillProxyPreparer::ensureFillProxy(
        tile,
        &device,
        4));
    EXPECT_EQ(4, device.createdBufferCount);

    tile.rasterOverlayState.ensureMapping(0);
    EXPECT_TRUE(TileFillProxyPreparer::ensureFillProxy(
        tile,
        &device,
        5));
    EXPECT_EQ(1u, tile.rasterOverlayState.mappingCount());
    EXPECT_EQ(6, device.createdBufferCount);

    tile.rasterOverlayState.ensureMapping(0);
    tile.key.schemeId = "XYZ-WebMercator";
    EXPECT_TRUE(TileFillProxyPreparer::ensureFillProxy(
        tile,
        &device,
        5));
    EXPECT_EQ(0u, tile.rasterOverlayState.mappingCount());
    EXPECT_EQ(8, device.createdBufferCount);
}

TEST(EllipsoidTerrainMeshBuilderTest,
     FillProxyValidatesSignatureAndResolvesTerrainProjection) {
    earth_engine::testing::MockRenderDevice device;
    TilesetTile tmsTile(
        TileKey{"TMS-WebMercator", 2, 1, 1},
        testRectangle());
    ASSERT_TRUE(TileFillProxyPreparer::ensureFillProxy(
        tmsTile,
        &device,
        0));
    const TileFillGeometrySignature* tmsSignature =
        tmsTile.content.renderContent.fillGeometrySignature();
    ASSERT_NE(nullptr, tmsSignature);
    EXPECT_EQ(RasterOverlayProjection::WebMercator,
              tmsSignature->projection);
    EXPECT_EQ(1, tmsSignature->gridSize);
    EXPECT_FALSE(TileFillProxyPreparer::ensureFillProxy(
        tmsTile,
        &device,
        1));

    TilesetTile openGlobusMercator(
        TileKey{"OpenGlobus-Earth", 2, 1, 3},
        testRectangle());
    ASSERT_TRUE(TileFillProxyPreparer::ensureFillProxy(
        openGlobusMercator,
        &device,
        1));
    ASSERT_NE(
        nullptr,
        openGlobusMercator.content.renderContent.fillGeometrySignature());
    EXPECT_EQ(
        RasterOverlayProjection::WebMercator,
        openGlobusMercator.content.renderContent
            .fillGeometrySignature()
            ->projection);

    TilesetTile openGlobusPolar(
        TileKey{"OpenGlobus-Earth", 2, 1, 4},
        testRectangle());
    ASSERT_TRUE(TileFillProxyPreparer::ensureFillProxy(
        openGlobusPolar,
        &device,
        1));
    ASSERT_NE(
        nullptr,
        openGlobusPolar.content.renderContent.fillGeometrySignature());
    EXPECT_EQ(
        RasterOverlayProjection::Geographic,
        openGlobusPolar.content.renderContent
            .fillGeometrySignature()
            ->projection);
}

TEST(EllipsoidTerrainMeshBuilderTest,
     FillProxyRejectsInvalidGeometryAndClearsStaleResources) {
    TilesetTile tile(
        TileKey{"Geographic-TMS", 2, 1, 1},
        testRectangle());
    earth_engine::testing::MockRenderDevice device;
    ASSERT_TRUE(TileFillProxyPreparer::ensureFillProxy(
        tile,
        &device,
        1));
    tile.rasterOverlayState.ensureMapping(0);

    tile.bounds = Rectangle(
        std::numeric_limits<double>::quiet_NaN(),
        0.0,
        1.0,
        1.0);
    const TileFillProxyPrepareResult invalidBounds =
        TileFillProxyPreparer::ensureFillProxy(
            tile,
            &device,
            1);
    EXPECT_FALSE(invalidBounds.madeReady);
    EXPECT_TRUE(invalidBounds.resourcesChanged);
    EXPECT_FALSE(tile.content.renderContent.isFillReady());
    EXPECT_EQ(0u, tile.rasterOverlayState.mappingCount());

    tile.bounds = testRectangle();
    const TileFillProxyPrepareResult oversizedGrid =
        TileFillProxyPreparer::ensureFillProxy(
            tile,
            &device,
            INT_MAX);
    EXPECT_FALSE(oversizedGrid.madeReady);
    EXPECT_FALSE(tile.content.renderContent.isFillReady());
}

TEST(EllipsoidTerrainMeshBuilderTest,
     FillProxySkipsReadySurfaceRenderContent) {
    TilesetTile tile(
        TileKey{"Geographic-TMS", 2, 1, 1},
        testRectangle());
    tile.content.renderContent.setMeshReady(true);
    earth_engine::testing::MockRenderDevice device;

    EXPECT_FALSE(TileFillProxyPreparer::ensureFillProxy(tile, &device, 4));
    EXPECT_FALSE(tile.content.renderContent.isFillReady());
    EXPECT_EQ(0, device.createdBufferCount);

    tile.content.renderContent.setMeshReady(false);
    ASSERT_TRUE(TileFillProxyPreparer::ensureFillProxy(tile, &device, 4));
    tile.content.renderContent.setMeshReady(true);
    EXPECT_FALSE(tile.content.renderContent.isFillReady());
    EXPECT_EQ(nullptr, tile.content.renderContent.fillGeometrySignature());
}

TEST(EllipsoidTerrainMeshBuilderTest,
     FillProxyUploadFailureDoesNotCommitPartialResourcesOrSignature) {
    TilesetTile tile(
        TileKey{"Geographic-TMS", 2, 1, 1},
        testRectangle());
    earth_engine::testing::MockRenderDevice device;
    device.failBufferCreationAtAttempt = 2;

    const TileFillProxyPrepareResult result =
        TileFillProxyPreparer::ensureFillProxy(
            tile,
            &device,
            4);

    EXPECT_FALSE(result.madeReady);
    EXPECT_FALSE(result.resourcesChanged);
    EXPECT_EQ(2, device.bufferCreationAttempts);
    EXPECT_EQ(1, device.createdBufferCount);
    EXPECT_FALSE(tile.content.renderContent.hasFillModel());
    EXPECT_FALSE(tile.content.renderContent.hasFillResources());
    EXPECT_FALSE(tile.content.renderContent.isFillReady());
    EXPECT_EQ(nullptr, tile.content.renderContent.fillGeometrySignature());
}
