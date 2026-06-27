#include <gtest/gtest.h>

#include "earth_engine/content/GltfModel.h"
#include "earth_engine/tiling/RasterMappedToTilesetTile.h"
#include "earth_engine/tiling/TileCacheMetrics.h"
#include "earth_engine/tiling/TilesetTile.h"

#include <cstdint>
#include <memory>
#include <unordered_map>

using namespace earth_engine;

namespace {

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
