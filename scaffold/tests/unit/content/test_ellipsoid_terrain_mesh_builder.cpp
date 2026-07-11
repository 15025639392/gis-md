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

#include <cmath>
#include <unordered_map>

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
    auto tile = std::make_unique<TilesetTile>(
        TileKey{"Geographic-TMS", 2, 1, 1},
        bounds);
    TilesetTile* tileForAssertions = tile.get();
    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;
    tiles.emplace("terrain", std::move(tile));
    earth_engine::testing::MockRenderDevice device;

    ASSERT_TRUE(TileFillProxyPreparer::ensureFillProxy(
        *tileForAssertions,
        tiles,
        &device,
        4));
    const TileRenderContentState& fillState =
        tileForAssertions->content.renderContent;
    ASSERT_TRUE(fillState.hasFillModel());
    ASSERT_TRUE(fillState.isFillReady());
    EXPECT_TRUE(fillState.drawsFill());
    ASSERT_EQ(1u, fillState.drawPrimitiveResources().size());
    EXPECT_TRUE(
        fillState.drawPrimitiveResources().front().useTerrainVertexFormat);
    EXPECT_EQ(2, device.createdBufferCount);

    auto realTerrain = EllipsoidTerrainMeshBuilder::makeModel(
        bounds,
        RasterOverlayProjection::Geographic,
        4);
    ASSERT_NE(nullptr, realTerrain);
    TileRenderContentState& realState =
        tileForAssertions->content.renderContent;
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
    ASSERT_EQ(1u, realState.drawPrimitiveResources().size());
    EXPECT_FALSE(
        realState.drawPrimitiveResources().front().useTerrainVertexFormat);
}
