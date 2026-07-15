#include <gtest/gtest.h>

#include "earth_engine/content/GltfModel.h"
#include "earth_engine/core/geodesy/Ellipsoid.h"
#include "earth_engine/core/geodesy/Projection.h"
#include "earth_engine/core/resources/FrameResourceBudget.h"
#include "earth_engine/layers/ActivatedRasterOverlay.h"
#include "earth_engine/layers/RasterOverlay.h"
#include "earth_engine/providers/DebugImageryProvider.h"
#include "earth_engine/providers/RasterOverlayTile.h"
#include "earth_engine/tiling/GltfRenderResourcePreparer.h"
#include "earth_engine/tiling/RasterMappedToTilesetTile.h"
#include "earth_engine/tiling/TileCacheMetrics.h"
#include "earth_engine/tiling/TileRasterOverlayPrefetcher.h"
#include "earth_engine/tiling/TileScheme.h"
#include "earth_engine/tiling/TilesetTile.h"

#include "../../helpers/MockRenderDevice.h"

#include <cstdint>
#include <memory>
#include <unordered_map>

using namespace earth_engine;

namespace {

class ExactTexture final : public Texture {
public:
    ExactTexture(int width, int height, size_t sizeBytes)
        : width_(width),
          height_(height),
          sizeBytes_(sizeBytes) {}

    int width() const override { return width_; }
    int height() const override { return height_; }
    size_t sizeBytes() const override { return sizeBytes_; }

private:
    int width_ = 0;
    int height_ = 0;
    size_t sizeBytes_ = 0;
};

std::unique_ptr<GltfModel> makeQuadTerrainGltfModel(
    const Rectangle& rectangle) {
    auto model = std::make_unique<GltfModel>();
    const Vec3 nodeOrigin(100.0, 200.0, 300.0);
    GltfNodeRuntime rootNode;
    rootNode.baseLocalTransform = Mat4::translation(nodeOrigin);
    rootNode.localTransform = rootNode.baseLocalTransform;
    rootNode.globalTransform = rootNode.baseLocalTransform;
    rootNode.mesh = 0;
    rootNode.hasMatrix = true;
    rootNode.baseTranslation = {nodeOrigin.x(), nodeOrigin.y(), nodeOrigin.z()};
    rootNode.translation = rootNode.baseTranslation;
    model->nodes.push_back(rootNode);
    model->sceneRootNodes.push_back(0);
    GltfPrimitive primitive;
    primitive.vertices.resize(4);
    primitive.vertices[0].positionEcef = nodeOrigin + Vec3(0.0, 0.0, 0.0);
    primitive.vertices[1].positionEcef = nodeOrigin + Vec3(2.0, 0.0, 0.0);
    primitive.vertices[2].positionEcef = nodeOrigin + Vec3(0.0, 2.0, 0.0);
    primitive.vertices[3].positionEcef = nodeOrigin + Vec3(2.0, 2.0, 0.0);
    for (SurfaceVertex& vertex : primitive.vertices) {
        vertex.normalEcef = Vec3::unitZ();
    }
    primitive.vertices[0].uv = {0.0f, 0.0f};
    primitive.vertices[1].uv = {1.0f, 0.0f};
    primitive.vertices[2].uv = {0.0f, 1.0f};
    primitive.vertices[3].uv = {1.0f, 1.0f};
    primitive.vertexTexCoords[0] = {
        std::array<float, 2>{0.0f, 0.0f},
        std::array<float, 2>{1.0f, 0.0f},
        std::array<float, 2>{0.0f, 1.0f},
        std::array<float, 2>{1.0f, 1.0f}};
    primitive.indices = {0, 1, 2, 1, 3, 2};
    primitive.runtime.baseVertices = primitive.vertices;
    for (SurfaceVertex& vertex : primitive.runtime.baseVertices) {
        vertex.positionEcef = vertex.positionEcef - nodeOrigin;
    }
    primitive.runtime.nodeIndex = 0;
    primitive.runtime.hasNormals = true;
    model->primitives.push_back(std::move(primitive));
    model->rasterOverlayDetails.setGeographicRectangle(rectangle);
    return model;
}

Rectangle projectForProvider(const TileScheme& scheme,
                             const Rectangle& geographicRectangle) {
    if (scheme.crsProfile() == "EPSG:3857") {
        return projectRectangleSimple(
            WebMercatorProjection(Ellipsoid::WGS84()),
            geographicRectangle);
    }
    return geographicRectangle;
}

RasterOverlayDetails makeProviderDetails(const TileScheme& scheme,
                                         const Rectangle& geographicRectangle) {
    RasterOverlayDetails details;
    if (scheme.crsProfile() == "EPSG:3857") {
        details.rasterOverlayProjections = {
            RasterOverlayProjection::WebMercator};
        details.rasterOverlayRectangles = {
            projectForProvider(scheme, geographicRectangle)};
        details.boundingRegion = {geographicRectangle, 0.0, 0.0};
    } else {
        details.setGeographicRectangle(geographicRectangle);
    }
    return details;
}

}  // namespace

TEST(TileCacheMetricsTest, EstimateTileBytesUsesActualMeshPayload) {
    TilesetTile tile;
    auto gltfModel = makeQuadTerrainGltfModel(tile.bounds);
    const int64_t expected = gltfModel->byteSize();
    tile.content.renderContent.prepareGltfContent(
        std::move(gltfModel), Mat4::identity());
    tile.content.renderContent.setTerrainRenderContent(true);
    tile.content.renderContent.addGltfPrimitiveResource(
        GltfPrimitiveRenderResources{});
    tile.content.renderContent.markRenderContentReady();

    EXPECT_EQ(expected, TileCacheMetrics::estimateTileBytes(tile));
}

TEST(
    TileCacheMetricsTest,
    RetainedBytesUseExactTextureAndAllPrimitiveBufferAllocations) {
    TileRenderContentState content;
    content.addGltfTextureResource(
        std::make_unique<ExactTexture>(16, 16, 37));

    GltfPrimitiveRenderResources gltfResources;
    gltfResources.vertexBuffer =
        std::make_unique<earth_engine::testing::DummyBuffer>(41);
    gltfResources.indexBuffer =
        std::make_unique<earth_engine::testing::DummyBuffer>(43);
    gltfResources.instanceBuffer =
        std::make_unique<earth_engine::testing::DummyBuffer>(47);
    content.addGltfPrimitiveResource(std::move(gltfResources));

    content.addFillTextureResource(
        std::make_unique<ExactTexture>(8, 8, 53));
    GltfPrimitiveRenderResources fillResources;
    fillResources.vertexBuffer =
        std::make_unique<earth_engine::testing::DummyBuffer>(59);
    fillResources.indexBuffer =
        std::make_unique<earth_engine::testing::DummyBuffer>(61);
    fillResources.instanceBuffer =
        std::make_unique<earth_engine::testing::DummyBuffer>(67);
    content.addFillPrimitiveResource(std::move(fillResources));

    EXPECT_EQ(
        37 + 41 + 43 + 47 + 53 + 59 + 61 + 67,
        content.estimateRetainedBytes());
}

TEST(TileCacheMetricsTest, GltfByteSizeCountsRuntimeAndPrebuiltTerrainBytes) {
    GltfModel model;
    GltfPrimitive primitive;
    primitive.vertices.resize(2);
    primitive.terrainGpuVertexBytes.resize(64);
    primitive.vertexTexCoords[0].resize(2);
    primitive.vertexColors.resize(2);
    primitive.vertexTangents.resize(2);
    primitive.indices.resize(3);
    primitive.featureIds.resize(2);
    primitive.runtime.baseVertices.resize(2);
    primitive.runtime.baseTangents.resize(2);
    primitive.runtime.skinning.resize(2);
    GltfMorphTarget morphTarget;
    morphTarget.positionDeltas.resize(2);
    morphTarget.normalDeltas.resize(1);
    morphTarget.tangentDeltas.resize(3);
    primitive.runtime.morphTargets.push_back(std::move(morphTarget));
    model.primitives.push_back(std::move(primitive));

    const int64_t expected =
        static_cast<int64_t>(2 * sizeof(SurfaceVertex)) +
        static_cast<int64_t>(64) +
        static_cast<int64_t>(2 * sizeof(std::array<float, 2>)) +
        static_cast<int64_t>(2 * sizeof(std::array<float, 4>)) +
        static_cast<int64_t>(2 * sizeof(std::array<float, 4>)) +
        static_cast<int64_t>(3 * sizeof(uint32_t)) +
        static_cast<int64_t>(2 * sizeof(uint32_t)) +
        static_cast<int64_t>(2 * sizeof(SurfaceVertex)) +
        static_cast<int64_t>(2 * sizeof(std::array<float, 4>)) +
        static_cast<int64_t>(2 * sizeof(GltfVertexSkinning)) +
        static_cast<int64_t>(6 * sizeof(Vec3));
    EXPECT_EQ(expected, model.byteSize());
}

TEST(TileCacheMetricsTest, ClearsPrebuiltTerrainBytesWithoutDroppingGeometry) {
    TilesetTile tile;
    auto gltfModel = makeQuadTerrainGltfModel(tile.bounds);
    ASSERT_FALSE(gltfModel->primitives.empty());
    gltfModel->primitives.front().terrainGpuVertexBytes.resize(128);
    const size_t vertexCount = gltfModel->primitives.front().vertices.size();
    const size_t baseVertexCount =
        gltfModel->primitives.front().runtime.baseVertices.size();
    tile.content.renderContent.prepareGltfContent(
        std::move(gltfModel), Mat4::identity());

    tile.content.renderContent.clearTerrainGpuVertexBytes();

    const GltfModel* retained = tile.content.renderContent.gltfModelForRead();
    ASSERT_NE(nullptr, retained);
    ASSERT_FALSE(retained->primitives.empty());
    EXPECT_TRUE(retained->primitives.front().terrainGpuVertexBytes.empty());
    EXPECT_EQ(vertexCount, retained->primitives.front().vertices.size());
    EXPECT_EQ(baseVertexCount,
              retained->primitives.front().runtime.baseVertices.size());
}

TEST(TileCacheMetricsTest, GpuUploadClearsRetainedPrebuiltTerrainBytes) {
    TilesetTile tile;
    auto gltfModel = makeQuadTerrainGltfModel(tile.bounds);
    ASSERT_FALSE(gltfModel->primitives.empty());
    gltfModel->primitives.front().terrainGpuVertexBytes.resize(128, 7);
    tile.content.renderContent.prepareGltfContent(
        std::move(gltfModel), Mat4::identity());
    tile.content.renderContent.setTerrainRenderContent(true);

    GpuReadyData ready;
    GpuReadyPrimitive primitive;
    primitive.vertexBytes.resize(128, 7);
    primitive.vertexStride = 32;
    primitive.vertexCount = 4;
    primitive.indices = {0, 1, 2, 1, 3, 2};
    primitive.indexCount = primitive.indices.size();
    primitive.metadata.useTerrainVertexFormat = true;
    ready.primitives.push_back(std::move(primitive));

    earth_engine::testing::MockRenderDevice device;
    ASSERT_TRUE(GltfRenderResourcePreparer::uploadToGpu(
        tile,
        &device,
        std::move(ready)));

    const GltfModel* retained = tile.content.renderContent.gltfModelForRead();
    ASSERT_NE(nullptr, retained);
    ASSERT_FALSE(retained->primitives.empty());
    EXPECT_TRUE(retained->primitives.front().terrainGpuVertexBytes.empty());
    EXPECT_TRUE(tile.content.renderContent.isGltfRenderReady());
    ASSERT_EQ(1u, tile.content.renderContent.gltfPrimitiveResourceCount());
    const GltfPrimitiveRenderResources* resources =
        tile.content.renderContent.gltfPrimitiveResourceForReadAt(0);
    ASSERT_NE(nullptr, resources);
    EXPECT_NE(nullptr, resources->vertexBuffer);
    EXPECT_NE(nullptr, resources->indexBuffer);
    EXPECT_EQ(4, resources->vertexCount);
    EXPECT_EQ(6, resources->indexCount);
}

TEST(TileCacheMetricsTest, CountsHeightmapAndRetainedTilePayloads) {
    DecodedHeightmap heightmap;
    heightmap.heights.resize(3);
    heightmap.noDataValues.resize(2);
    heightmap.metadataAvailability.resize(4);

    const int64_t expectedHeightmapBytes =
        static_cast<int64_t>(3 * sizeof(float)) +
        static_cast<int64_t>(2 * sizeof(float)) +
        static_cast<int64_t>(
            4 * sizeof(QuantizedMeshAvailabilityRange));
    EXPECT_EQ(expectedHeightmapBytes,
              TileCacheMetrics::estimateHeightmapBytes(heightmap));

    TilesetTile tile;
    tile.key = TileKey{"test", 0, 0, 0};
    auto gltfModel = makeQuadTerrainGltfModel(tile.bounds);
    const int64_t gltfBytes = gltfModel->byteSize();
    tile.content.renderContent.prepareGltfContent(
        std::move(gltfModel), Mat4::identity());
    tile.content.renderContent.setTerrainRenderContent(true);
    tile.content.renderContent.addGltfPrimitiveResource(
        GltfPrimitiveRenderResources{});
    tile.content.renderContent.markRenderContentReady();

    const int64_t expectedTileBytes = gltfBytes;
    EXPECT_EQ(expectedTileBytes, TileCacheMetrics::estimateTileBytes(tile));
}

TEST(TileCacheMetricsTest, TotalsTileAndTerrainCachePayloads) {
    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;
    auto tile = std::make_unique<TilesetTile>(
        TileKey{"test", 0, 0, 0},
        Rectangle{});
    auto gltfModel = makeQuadTerrainGltfModel(tile->bounds);
    const int64_t expectedTileBytes = gltfModel->byteSize();
    tile->content.renderContent.prepareGltfContent(
        std::move(gltfModel), Mat4::identity());
    tile->content.renderContent.setTerrainRenderContent(true);
    tile->content.renderContent.addGltfPrimitiveResource(
        GltfPrimitiveRenderResources{});
    tile->content.renderContent.markRenderContentReady();
    tiles["tile"] = std::move(tile);
    tiles["null-tile"] = nullptr;

    std::unordered_map<std::string, std::unique_ptr<DecodedHeightmap>>
        terrainCache;
    auto heightmap = std::make_unique<DecodedHeightmap>();
    heightmap->metadataAvailability.resize(1);
    const int64_t expectedHeightmapBytes =
        static_cast<int64_t>(sizeof(QuantizedMeshAvailabilityRange));
    terrainCache["terrain"] = std::move(heightmap);
    terrainCache["null-terrain"] = nullptr;

    EXPECT_EQ(
        expectedTileBytes + expectedHeightmapBytes,
        TileCacheMetrics::estimateTotalBytes(tiles, terrainCache));
}

TEST(TileCacheMetricsTest, SharedAncestorRasterTextureCountsOnlyOnceGlobally) {
    auto overlay = std::make_unique<RasterOverlay>(
        std::make_unique<DebugImageryProvider>(),
        TileScheme::createXYZWebMercator(),
        RasterOverlay::Options{});
    ActivatedRasterOverlay activated(*overlay);
    RasterOverlayTileProvider* provider = activated.ensureTileProvider(nullptr);
    ASSERT_NE(nullptr, provider);

    const TileKey parentKey{overlay->getTileScheme().id(), 2, 1, 1};
    const TileKey childKey{overlay->getTileScheme().id(), 3, 2, 2};
    const Rectangle parentBounds =
        overlay->getTileScheme().tileToRectangle(parentKey);
    const Rectangle childBounds =
        overlay->getTileScheme().tileToRectangle(childKey);
    RasterOverlayDetails parentDetails =
        makeProviderDetails(overlay->getTileScheme(), parentBounds);
    RasterOverlayDetails childDetails =
        makeProviderDetails(overlay->getTileScheme(), childBounds);
    std::vector<RasterOverlayProjection> missing;

    auto parentTile = std::make_unique<TilesetTile>(parentKey, parentBounds);
    RasterMappedToTilesetTile& parentMapping =
        parentTile->rasterOverlayState.ensureMapping(0);
    parentMapping.update(
        parentKey,
        parentDetails,
        512.0,
        512.0,
        *provider,
        nullptr,
        missing,
        nullptr,
        0);
    RasterOverlayTile* parentRaster = parentMapping.getLoadingTile();
    ASSERT_NE(nullptr, parentRaster);
    parentRaster->setTexture(std::make_unique<earth_engine::testing::DummyTexture>(
        4,
        4));
    parentMapping.update(
        parentKey,
        parentDetails,
        512.0,
        512.0,
        *provider,
        nullptr,
        missing,
        nullptr,
        0);
    ASSERT_EQ(parentRaster, parentMapping.getReadyTile());

    auto childTile =
        std::make_unique<TilesetTile>(childKey, childBounds, parentTile.get());
    childTile->geometricError = 100.0;
    RasterMappedToTilesetTile& childMapping =
        childTile->rasterOverlayState.ensureMapping(0);
    childMapping.update(
        childKey,
        childDetails,
        512.0,
        512.0,
        *provider,
        nullptr,
        missing,
        parentTile.get(),
        0);

    FrameResourceBudgetConfig config;
    config.maxRasterNetworkRequestsPerFrame = 64;
    config.maxRasterNetworkInflight = 64;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);
    std::vector<ActivatedRasterOverlay*> overlays{&activated};
    TileRasterOverlayPrefetcher::prefetch(
        *childTile,
        overlays,
        {0},
        nullptr,
        16.0,
        budget);

    ASSERT_EQ(parentRaster, childMapping.getReadyTile());
    ASSERT_EQ(RasterMappedToTilesetTile::ReadyTileSource::Ancestor,
              childMapping.getReadyTileSource());

    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;
    tiles.emplace("parent", std::move(parentTile));
    tiles.emplace("child", std::move(childTile));

    EXPECT_EQ(4 * 4 * 4,
              TileCacheMetrics::estimateTotalBytes(tiles, {}));
}
