#include <gtest/gtest.h>

#include "earth_engine/content/QuantizedMeshContentLoader.h"
#include "earth_engine/core/geodesy/Cartographic.h"
#include "earth_engine/core/geodesy/Ellipsoid.h"
#include "earth_engine/core/geodesy/Projection.h"
#include "earth_engine/core/math/Vec3.h"
#include "earth_engine/tiling/TileLoadTypes.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <tuple>
#include <vector>

using namespace earth_engine;

namespace {

template <typename T>
void appendPod(std::vector<uint8_t>& bytes, T value) {
    const auto* p = reinterpret_cast<const uint8_t*>(&value);
    bytes.insert(bytes.end(), p, p + sizeof(T));
}

uint16_t zigZagEncode16(int32_t value) {
    return static_cast<uint16_t>(
        value >= 0 ? value * 2 : (-value * 2) - 1);
}

std::vector<uint8_t> makeQuantizedMeshBytes(
    float minimumHeight = 0.0f,
    float maximumHeight = 100.0f,
    const Vec3& horizonOcclusionPoint = Vec3::zero(),
    const Vec3& boundingSphereCenter = Vec3::zero()) {
    std::vector<uint8_t> bytes;

    for (int i = 0; i < 3; ++i) appendPod<double>(bytes, 0.0);
    appendPod<float>(bytes, minimumHeight);
    appendPod<float>(bytes, maximumHeight);
    appendPod<double>(bytes, boundingSphereCenter.x());
    appendPod<double>(bytes, boundingSphereCenter.y());
    appendPod<double>(bytes, boundingSphereCenter.z());
    appendPod<double>(bytes, 0.0);
    appendPod<double>(bytes, horizonOcclusionPoint.x());
    appendPod<double>(bytes, horizonOcclusionPoint.y());
    appendPod<double>(bytes, horizonOcclusionPoint.z());
    appendPod<uint32_t>(bytes, 3);

    const uint16_t u[] = {
        zigZagEncode16(0),
        zigZagEncode16(32767),
        zigZagEncode16(-32767)
    };
    const uint16_t v[] = {
        zigZagEncode16(0),
        zigZagEncode16(0),
        zigZagEncode16(32767)
    };
    const uint16_t h[] = {
        zigZagEncode16(0),
        zigZagEncode16(32767),
        zigZagEncode16(0)
    };
    for (uint16_t value : u) appendPod<uint16_t>(bytes, value);
    for (uint16_t value : v) appendPod<uint16_t>(bytes, value);
    for (uint16_t value : h) appendPod<uint16_t>(bytes, value);

    appendPod<uint32_t>(bytes, 1);
    for (int i = 0; i < 3; ++i) appendPod<uint16_t>(bytes, 0);
    for (int i = 0; i < 4; ++i) appendPod<uint32_t>(bytes, 0);
    return bytes;
}

std::vector<uint8_t> makeHighTriangleCountQuantizedMeshBytes() {
    std::vector<uint8_t> bytes;

    for (int i = 0; i < 3; ++i) appendPod<double>(bytes, 0.0);
    appendPod<float>(bytes, 0.0f);
    appendPod<float>(bytes, 100.0f);
    for (int i = 0; i < 7; ++i) appendPod<double>(bytes, 0.0);
    appendPod<uint32_t>(bytes, 3);

    const uint16_t u[] = {
        zigZagEncode16(0),
        zigZagEncode16(32767),
        zigZagEncode16(-32767)
    };
    const uint16_t v[] = {
        zigZagEncode16(0),
        zigZagEncode16(0),
        zigZagEncode16(32767)
    };
    const uint16_t h[] = {
        zigZagEncode16(0),
        zigZagEncode16(0),
        zigZagEncode16(0)
    };
    for (uint16_t value : u) appendPod<uint16_t>(bytes, value);
    for (uint16_t value : v) appendPod<uint16_t>(bytes, value);
    for (uint16_t value : h) appendPod<uint16_t>(bytes, value);

    constexpr uint32_t kTriangleCount = 13;
    appendPod<uint32_t>(bytes, kTriangleCount);
    const uint16_t firstTriangle[] = {0, 0, 0};
    for (uint16_t value : firstTriangle) appendPod<uint16_t>(bytes, value);
    for (uint32_t triangle = 1; triangle < kTriangleCount; ++triangle) {
        const uint16_t repeatedTriangle[] = {3, 2, 1};
        for (uint16_t value : repeatedTriangle) appendPod<uint16_t>(bytes, value);
    }
    for (int i = 0; i < 4; ++i) appendPod<uint32_t>(bytes, 0);
    return bytes;
}

std::vector<uint8_t> makeQuadQuantizedMeshBytesWithEdges() {
    std::vector<uint8_t> bytes;

    for (int i = 0; i < 3; ++i) appendPod<double>(bytes, 0.0);
    appendPod<float>(bytes, 0.0f);
    appendPod<float>(bytes, 100.0f);
    for (int i = 0; i < 7; ++i) appendPod<double>(bytes, 0.0);
    appendPod<uint32_t>(bytes, 4);

    const uint16_t u[] = {
        zigZagEncode16(0),
        zigZagEncode16(32767),
        zigZagEncode16(-32767),
        zigZagEncode16(32767)
    };
    const uint16_t v[] = {
        zigZagEncode16(0),
        zigZagEncode16(0),
        zigZagEncode16(32767),
        zigZagEncode16(0)
    };
    const uint16_t h[] = {
        zigZagEncode16(0),
        zigZagEncode16(0),
        zigZagEncode16(0),
        zigZagEncode16(0)
    };
    for (uint16_t value : u) appendPod<uint16_t>(bytes, value);
    for (uint16_t value : v) appendPod<uint16_t>(bytes, value);
    for (uint16_t value : h) appendPod<uint16_t>(bytes, value);

    appendPod<uint32_t>(bytes, 2);
    const uint16_t encodedIndices[] = {0, 0, 0, 2, 0, 2};
    for (uint16_t value : encodedIndices) appendPod<uint16_t>(bytes, value);

    const std::vector<uint16_t> west{0, 2};
    const std::vector<uint16_t> south{1, 0};
    const std::vector<uint16_t> east{3, 1};
    const std::vector<uint16_t> north{2, 3};
    for (const std::vector<uint16_t>* edge : {&west, &south, &east, &north}) {
        appendPod<uint32_t>(bytes, static_cast<uint32_t>(edge->size()));
        for (uint16_t index : *edge) appendPod<uint16_t>(bytes, index);
    }

    return bytes;
}

std::vector<uint8_t> makeQuantizedMeshBytesWithInteriorVertex(
    const Vec3& boundingSphereCenter = Vec3::zero()) {
    std::vector<uint8_t> bytes;

    for (int i = 0; i < 3; ++i) appendPod<double>(bytes, 0.0);
    appendPod<float>(bytes, 0.0f);
    appendPod<float>(bytes, 100.0f);
    appendPod<double>(bytes, boundingSphereCenter.x());
    appendPod<double>(bytes, boundingSphereCenter.y());
    appendPod<double>(bytes, boundingSphereCenter.z());
    appendPod<double>(bytes, 0.0);
    for (int i = 0; i < 3; ++i) appendPod<double>(bytes, 0.0);
    appendPod<uint32_t>(bytes, 4);

    const uint16_t u[] = {
        zigZagEncode16(0),
        zigZagEncode16(32767),
        zigZagEncode16(-32767),
        zigZagEncode16(16384)
    };
    const uint16_t v[] = {
        zigZagEncode16(0),
        zigZagEncode16(0),
        zigZagEncode16(32767),
        zigZagEncode16(-24575)
    };
    const uint16_t h[] = {
        zigZagEncode16(0),
        zigZagEncode16(0),
        zigZagEncode16(0),
        zigZagEncode16(0)
    };
    for (uint16_t value : u) appendPod<uint16_t>(bytes, value);
    for (uint16_t value : v) appendPod<uint16_t>(bytes, value);
    for (uint16_t value : h) appendPod<uint16_t>(bytes, value);

    appendPod<uint32_t>(bytes, 2);
    const uint16_t encodedIndices[] = {0, 0, 0, 2, 0, 2};
    for (uint16_t value : encodedIndices) appendPod<uint16_t>(bytes, value);

    const std::vector<uint16_t> west{0, 2};
    const std::vector<uint16_t> south{1, 0};
    const std::vector<uint16_t> east{1, 3};
    const std::vector<uint16_t> north{2, 3};
    for (const std::vector<uint16_t>* edge : {&west, &south, &east, &north}) {
        appendPod<uint32_t>(bytes, static_cast<uint32_t>(edge->size()));
        for (uint16_t index : *edge) appendPod<uint16_t>(bytes, index);
    }

    return bytes;
}

std::vector<uint8_t> makeQuantizedMeshBytesWithMetadata(
    const std::string& metadataJson,
    float minimumHeight = 0.0f,
    float maximumHeight = 100.0f) {
    std::vector<uint8_t> bytes =
        makeQuantizedMeshBytes(minimumHeight, maximumHeight);

    appendPod<uint8_t>(bytes, 4);
    appendPod<uint32_t>(
        bytes,
        static_cast<uint32_t>(sizeof(uint32_t) + metadataJson.size()));
    appendPod<uint32_t>(bytes, static_cast<uint32_t>(metadataJson.size()));
    bytes.insert(bytes.end(), metadataJson.begin(), metadataJson.end());
    return bytes;
}

std::vector<uint8_t> makeQuantizedMeshBytesWithWaterMask(
    const std::vector<uint8_t>& waterMask,
    float minimumHeight = 0.0f,
    float maximumHeight = 100.0f) {
    std::vector<uint8_t> bytes =
        makeQuantizedMeshBytes(minimumHeight, maximumHeight);

    appendPod<uint8_t>(bytes, 2);
    appendPod<uint32_t>(bytes, static_cast<uint32_t>(waterMask.size()));
    bytes.insert(bytes.end(), waterMask.begin(), waterMask.end());
    return bytes;
}

Rectangle geographicRootWestRectangle() {
    constexpr double kPi = 3.14159265358979323846264338327950288;
    return Rectangle(-kPi, -0.5 * kPi, 0.0, 0.5 * kPi);
}

} // namespace

TEST(QuantizedMeshContentLoaderTest,
     CarriesMixedWaterMaskIntoGltfTerrainModel) {
    std::vector<uint8_t> waterMask(256u * 256u, 0u);
    waterMask[0] = 7u;
    waterMask[12345] = 255u;
    const std::vector<uint8_t> bytes =
        makeQuantizedMeshBytesWithWaterMask(waterMask, -10.0f, 150.0f);

    QuantizedMeshContentLoadResult result =
        QuantizedMeshContentLoader::load(
            bytes.data(),
            bytes.size(),
            geographicRootWestRectangle(),
            true,
            {});

    ASSERT_TRUE(result.success());
    ASSERT_NE(nullptr, result.gltfModel);
    EXPECT_FALSE(result.gltfModel->terrainWaterMask.allLand);
    EXPECT_FALSE(result.gltfModel->terrainWaterMask.allWater);
    EXPECT_TRUE(result.gltfModel->terrainWaterMask.valid());
    ASSERT_TRUE(result.gltfModel->terrainWaterMaskTextureIndex.has_value());
    ASSERT_EQ(1u, result.gltfModel->primitives.size());
    EXPECT_TRUE(
        result.gltfModel->primitives.front().hasTerrainWaterMaskMetadata);
    ASSERT_EQ(1u, result.gltfModel->textures.size());
    const GltfTexture& texture = result.gltfModel->textures.front();
    EXPECT_EQ(256, texture.image.width);
    EXPECT_EQ(256, texture.image.height);
    EXPECT_EQ(1, texture.image.channels);
    ASSERT_EQ(256u * 256u, texture.image.pixels.size());
    EXPECT_EQ(7u, texture.image.pixels[0]);
    EXPECT_EQ(255u, texture.image.pixels[12345u]);
    EXPECT_EQ(GltfTextureWrap::ClampToEdge, texture.sampler.wrapS);
    EXPECT_EQ(GltfTextureWrap::ClampToEdge, texture.sampler.wrapT);
    EXPECT_TRUE(texture.sampler.mipmap);
}

TEST(QuantizedMeshContentLoaderTest,
     CarriesSingleByteWaterMaskFlagsWithoutTextureLikeCesiumNative) {
    for (const auto& [maskValue, expectedOnlyWater, expectedOnlyLand] :
         std::array<std::tuple<uint8_t, bool, bool>, 2>{
             std::tuple<uint8_t, bool, bool>{0u, false, true},
             std::tuple<uint8_t, bool, bool>{255u, true, false}}) {
        const std::vector<uint8_t> bytes =
            makeQuantizedMeshBytesWithWaterMask({maskValue}, -10.0f, 150.0f);

        QuantizedMeshContentLoadResult result =
            QuantizedMeshContentLoader::load(
                bytes.data(),
                bytes.size(),
                geographicRootWestRectangle(),
                true,
                {});

        ASSERT_TRUE(result.success());
        ASSERT_NE(nullptr, result.gltfModel);
        EXPECT_EQ(expectedOnlyWater,
                  result.gltfModel->terrainWaterMask.allWater);
        EXPECT_EQ(expectedOnlyLand,
                  result.gltfModel->terrainWaterMask.allLand);
        EXPECT_TRUE(result.gltfModel->terrainWaterMask.data.empty());
        EXPECT_FALSE(result.gltfModel->terrainWaterMaskTextureIndex.has_value());
        EXPECT_TRUE(result.gltfModel->textures.empty());
        ASSERT_EQ(1u, result.gltfModel->primitives.size());
        const GltfPrimitive& primitive = result.gltfModel->primitives.front();
        EXPECT_TRUE(primitive.hasTerrainWaterMaskMetadata);
        EXPECT_EQ(expectedOnlyWater, primitive.terrainOnlyWater);
        EXPECT_EQ(expectedOnlyLand, primitive.terrainOnlyLand);
        EXPECT_FALSE(primitive.terrainWaterMaskTextureIndex.has_value());
        EXPECT_DOUBLE_EQ(0.0, primitive.terrainWaterMaskTranslationX);
        EXPECT_DOUBLE_EQ(0.0, primitive.terrainWaterMaskTranslationY);
        EXPECT_DOUBLE_EQ(1.0, primitive.terrainWaterMaskScale);
    }
}

TEST(QuantizedMeshContentLoaderTest,
     LoadsTileContentResultForUnifiedTerrainLifecycle) {
    const std::string metadataJson = R"json({
        "available": [
            [
                {"startX": 0, "startY": 0, "endX": 1, "endY": 1}
            ]
        ]
    })json";
    const std::vector<uint8_t> bytes =
        makeQuantizedMeshBytesWithMetadata(metadataJson, -10.0f, 150.0f);
    QuantizedMeshAvailabilityUpdate currentTileUpdate;
    currentTileUpdate.layerIndex = 2;
    currentTileUpdate.subtreeKey = TileKey{"Geographic-TMS", 1, 0, 0};

    TileContentLoadResult result =
        QuantizedMeshContentLoader::loadTileContent(
            bytes.data(),
            bytes.size(),
            geographicRootWestRectangle(),
            false,
            {},
            RasterOverlayProjection::Geographic,
            currentTileUpdate,
            QuantizedMeshContentLoader::RasterOverlayDetailsMode::
                GenerateTerrainProjection);

    EXPECT_EQ(TileLoadStatus::Renderable, result.status);
    EXPECT_TRUE(result.terrainRenderContent);
    ASSERT_NE(nullptr, result.gltfModel);
    EXPECT_FALSE(result.gltfModel->primitives.empty());
    ASSERT_TRUE(result.metadata.updatedBoundingVolume.has_value());
    EXPECT_EQ(geographicRootWestRectangle(),
              result.metadata.updatedBoundingVolume->region);
    ASSERT_TRUE(result.metadata.rasterOverlayDetails.has_value());
    EXPECT_TRUE(result.metadata.rasterOverlayDetails->equalsExact(
        result.gltfModel->rasterOverlayDetails));
    const Rectangle* rasterRectangle =
        result.metadata.rasterOverlayDetails->findRectangleForOverlayProjection(
            RasterOverlayProjection::Geographic);
    ASSERT_NE(nullptr, rasterRectangle);
    EXPECT_EQ(geographicRootWestRectangle(), *rasterRectangle);
    ASSERT_EQ(1u, result.quantizedMeshAvailabilityUpdates.size());
    EXPECT_EQ(2, result.quantizedMeshAvailabilityUpdates.front().layerIndex);
    EXPECT_EQ(currentTileUpdate.subtreeKey,
              result.quantizedMeshAvailabilityUpdates.front().subtreeKey);

    TileLoadResult loadResult =
        TileLoadResult::fromContentResult(std::move(result));
    EXPECT_EQ(TileLoadStatus::Renderable, loadResult.status);
    EXPECT_TRUE(loadResult.shouldUpload());
    EXPECT_TRUE(loadResult.isRenderableContentTerrain());
    EXPECT_TRUE(loadResult.content.satisfiesContentTerrainPayloadContract());
}

TEST(QuantizedMeshContentLoaderTest,
     LoadsGltfTerrainModelWithoutTerrainProvider) {
    const Vec3 boundingSphereCenter(10.0, 20.0, 30.0);
    const std::vector<uint8_t> bytes =
        makeQuantizedMeshBytes(
            -10.0f,
            150.0f,
            Vec3::zero(),
            boundingSphereCenter);

    QuantizedMeshContentLoadResult result =
        QuantizedMeshContentLoader::load(
            bytes.data(),
            bytes.size(),
            geographicRootWestRectangle(),
            false,
            {},
            RasterOverlayProjection::Geographic,
            std::nullopt,
            QuantizedMeshContentLoader::RasterOverlayDetailsMode::
                GenerateTerrainProjection);

    EXPECT_TRUE(result.success());
    ASSERT_NE(nullptr, result.gltfModel);
    ASSERT_TRUE(result.gltfModel->preferredLocalOriginEcef.has_value());
    EXPECT_EQ(boundingSphereCenter,
              *result.gltfModel->preferredLocalOriginEcef);
    ASSERT_EQ(1u, result.gltfModel->nodes.size());
    ASSERT_EQ(1u, result.gltfModel->sceneRootNodes.size());
    EXPECT_EQ(0, result.gltfModel->sceneRootNodes.front());
    const GltfNodeRuntime& node = result.gltfModel->nodes.front();
    EXPECT_EQ(0, node.mesh);
    EXPECT_EQ(-1, node.parent);
    EXPECT_TRUE(node.children.empty());
    EXPECT_EQ(Mat4::translation(boundingSphereCenter), node.baseLocalTransform);
    EXPECT_EQ(Mat4::translation(boundingSphereCenter), node.localTransform);
    EXPECT_EQ(Mat4::translation(boundingSphereCenter), node.globalTransform);
    ASSERT_EQ(1u, result.gltfModel->primitives.size());
    const GltfPrimitive& primitive = result.gltfModel->primitives.front();
    EXPECT_EQ(0, primitive.runtime.nodeIndex);
    EXPECT_EQ(GltfPrimitiveMode::Triangles, primitive.primitiveMode);
    EXPECT_FALSE(primitive.doubleSided);
    EXPECT_NEAR(0.0f, primitive.metallicFactor, 1e-6f);
    EXPECT_NEAR(1.0f, primitive.roughnessFactor, 1e-6f);
    EXPECT_FALSE(primitive.vertices.empty());
    EXPECT_FALSE(primitive.indices.empty());
    ASSERT_EQ(primitive.vertices.size(), primitive.vertexTexCoords[0].size());
    EXPECT_EQ(geographicRootWestRectangle(),
              result.gltfModel->rasterOverlayDetails.boundingRegion
                  .rectangle);
    const Rectangle* modelRasterRectangle =
        result.gltfModel->rasterOverlayDetails
            .findRectangleForOverlayProjection(
                RasterOverlayProjection::Geographic);
    ASSERT_NE(nullptr, modelRasterRectangle);
    EXPECT_EQ(geographicRootWestRectangle(), *modelRasterRectangle);
    EXPECT_FALSE(primitive.vertices.empty());
    EXPECT_FALSE(primitive.indices.empty());
    // 原生 uv 已是 NW 约定（QuantizedMeshParser 解码翻转），set 0
    // 直接拷贝；Geographic 地形投影不做进一步重写。
    EXPECT_EQ(primitive.vertices.front().uv,
              primitive.vertexTexCoords[0].front());
    EXPECT_EQ(primitive.vertices.back().uv[0],
              primitive.vertexTexCoords[0].back()[0]);
    EXPECT_EQ(primitive.vertices.back().uv[1],
              primitive.vertexTexCoords[0].back()[1]);
    ASSERT_EQ(primitive.vertices.size(), primitive.runtime.baseVertices.size());
    EXPECT_LT((primitive.runtime.baseVertices.front().positionEcef -
               (primitive.vertices.front().positionEcef -
                boundingSphereCenter))
                  .length(),
              1e-9);
    EXPECT_LT((primitive.vertices.front().positionEcef -
               (node.globalTransform *
                primitive.runtime.baseVertices.front().positionEcef))
                  .length(),
              1e-9);
    EXPECT_EQ(primitive.vertices.front().normalEcef,
              primitive.runtime.baseVertices.front().normalEcef);
    ASSERT_TRUE(primitive.skirtMetadata.has_value());
    EXPECT_EQ(0u, primitive.skirtMetadata->noSkirtIndicesBegin);
    EXPECT_EQ(primitive.indices.size(),
              primitive.skirtMetadata->noSkirtIndicesCount);
    EXPECT_EQ(0u, primitive.skirtMetadata->noSkirtVerticesBegin);
    EXPECT_EQ(primitive.vertices.size(),
              primitive.skirtMetadata->noSkirtVerticesCount);
    EXPECT_GT(primitive.skirtMetadata->skirtWestHeight, 0.0);
    EXPECT_DOUBLE_EQ(primitive.skirtMetadata->skirtWestHeight,
                     primitive.skirtMetadata->skirtSouthHeight);
    EXPECT_DOUBLE_EQ(primitive.skirtMetadata->skirtWestHeight,
                     primitive.skirtMetadata->skirtEastHeight);
    EXPECT_DOUBLE_EQ(primitive.skirtMetadata->skirtWestHeight,
                     primitive.skirtMetadata->skirtNorthHeight);
    ASSERT_TRUE(result.metadata.rasterOverlayDetails.has_value());
    const Rectangle* rasterRectangle =
        result.metadata.rasterOverlayDetails->findRectangleForOverlayProjection(
            RasterOverlayProjection::Geographic);
    ASSERT_NE(nullptr, rasterRectangle);
    EXPECT_EQ(geographicRootWestRectangle(), *rasterRectangle);
    EXPECT_EQ(geographicRootWestRectangle(),
              result.metadata.rasterOverlayDetails->boundingRegion.rectangle);
    EXPECT_NEAR(-10.0,
                result.metadata.rasterOverlayDetails->boundingRegion
                    .minimumHeight,
                1e-6);
    EXPECT_NEAR(150.0,
                result.metadata.rasterOverlayDetails->boundingRegion
                    .maximumHeight,
                1e-6);
    ASSERT_TRUE(result.metadata.updatedBoundingVolume.has_value());
    EXPECT_EQ(TileBoundingVolumeKind::Region,
              result.metadata.updatedBoundingVolume->kind);
    EXPECT_EQ(geographicRootWestRectangle(),
              result.metadata.updatedBoundingVolume->region);
    EXPECT_NEAR(-10.0,
                result.metadata.updatedBoundingVolume->minimumHeight,
                1e-6);
    EXPECT_NEAR(150.0,
                result.metadata.updatedBoundingVolume->maximumHeight,
                1e-6);
    ASSERT_TRUE(result.metadata.terrainHeightRange.has_value());
    EXPECT_NEAR(-10.0, result.metadata.terrainHeightRange->first, 1e-6);
    EXPECT_NEAR(150.0, result.metadata.terrainHeightRange->second, 1e-6);
    EXPECT_FALSE(result.metadata.updatedContentBoundingVolume.has_value());
    EXPECT_TRUE(result.availabilityUpdates.empty());
}

TEST(QuantizedMeshContentLoaderTest,
     CanLoadGltfTerrainWithoutRasterOverlayDetailsLikeLayerJsonTerrainLoader) {
    const std::vector<uint8_t> bytes =
        makeQuantizedMeshBytes(-10.0f, 150.0f);

    QuantizedMeshContentLoadResult result =
        QuantizedMeshContentLoader::load(
            bytes.data(),
            bytes.size(),
            geographicRootWestRectangle(),
            false,
            {},
            RasterOverlayProjection::Geographic,
            std::nullopt);

    ASSERT_TRUE(result.success());
    ASSERT_NE(nullptr, result.gltfModel);
    EXPECT_TRUE(result.gltfModel->rasterOverlayDetails.empty());
    EXPECT_FALSE(result.metadata.rasterOverlayDetails.has_value());
    ASSERT_TRUE(result.metadata.updatedBoundingVolume.has_value());
    EXPECT_EQ(geographicRootWestRectangle(),
              result.metadata.updatedBoundingVolume->region);
    ASSERT_TRUE(result.metadata.terrainHeightRange.has_value());
    EXPECT_NEAR(-10.0, result.metadata.terrainHeightRange->first, 1e-6);
    EXPECT_NEAR(150.0, result.metadata.terrainHeightRange->second, 1e-6);

    TileContentLoadResult contentResult =
        QuantizedMeshContentLoader::toTileContentLoadResult(
            std::move(result));
    EXPECT_EQ(TileLoadStatus::Renderable, contentResult.status);
    EXPECT_TRUE(contentResult.terrainRenderContent);
    ASSERT_NE(nullptr, contentResult.gltfModel);
    EXPECT_TRUE(contentResult.gltfModel->rasterOverlayDetails.empty());
    EXPECT_FALSE(contentResult.metadata.rasterOverlayDetails.has_value());

    TileLoadResult loadResult =
        TileLoadResult::fromContentResult(std::move(contentResult));
    EXPECT_EQ(TileLoadStatus::Renderable, loadResult.status);
    EXPECT_TRUE(loadResult.shouldUpload());
    EXPECT_TRUE(loadResult.isRenderableContentTerrain());
    EXPECT_TRUE(loadResult.content.satisfiesContentTerrainPayloadContract());
    EXPECT_TRUE(loadResult.content.hasGltfTerrainPayload());
    EXPECT_TRUE(loadResult.content.gltfModel->rasterOverlayDetails.empty());
    EXPECT_FALSE(loadResult.content.metadata.rasterOverlayDetails.has_value());
}

TEST(QuantizedMeshContentLoaderTest,
     GeneratesWebMercatorRasterOverlayDetailsLikeLayerJsonTerrainLoader) {
    const std::vector<uint8_t> bytes =
        makeQuantizedMeshBytesWithInteriorVertex();
    const Rectangle tileRectangle = geographicRootWestRectangle();

    QuantizedMeshContentLoadResult result =
        QuantizedMeshContentLoader::load(
            bytes.data(),
            bytes.size(),
            tileRectangle,
            false,
            {},
            RasterOverlayProjection::WebMercator,
            std::nullopt,
            QuantizedMeshContentLoader::RasterOverlayDetailsMode::
                GenerateTerrainProjection);

    ASSERT_TRUE(result.success());
    ASSERT_NE(nullptr, result.gltfModel);
    ASSERT_EQ(1u, result.gltfModel->primitives.size());

    const WebMercatorProjection projection(Ellipsoid::WGS84());
    const Rectangle expectedProjectedRectangle = projectRectangleSimple(
        projection,
        tileRectangle);
    const Rectangle* modelRasterRectangle =
        result.gltfModel->rasterOverlayDetails
            .findRectangleForOverlayProjection(
                RasterOverlayProjection::WebMercator);
    ASSERT_NE(nullptr, modelRasterRectangle);
    EXPECT_TRUE(modelRasterRectangle->equalsEpsilon(
        expectedProjectedRectangle,
        1e-6));
    EXPECT_EQ(nullptr,
              result.gltfModel->rasterOverlayDetails
                  .findRectangleForOverlayProjection(
                      RasterOverlayProjection::Geographic));
    EXPECT_EQ(tileRectangle,
              result.gltfModel->rasterOverlayDetails.boundingRegion
                  .rectangle);
    EXPECT_EQ(0,
              result.gltfModel->rasterOverlayDetails
                  .textureCoordinateIDForProjection(
                      RasterOverlayProjection::WebMercator));

    const GltfPrimitive& primitive = result.gltfModel->primitives.front();
    ASSERT_EQ(primitive.vertices.size(), primitive.vertexTexCoords[0].size());
    ASSERT_GT(primitive.vertices.size(), 3u);
    const size_t interiorVertexIndex = 3u;
    const Cartographic interiorCartographic =
        Ellipsoid::WGS84().cartesianToCartographic(
            primitive.vertices[interiorVertexIndex].positionEcef);
    const Vec3 projectedInterior =
        projectPosition(projection, interiorCartographic);
    const double expectedU =
        (projectedInterior.x() - expectedProjectedRectangle.west()) /
        expectedProjectedRectangle.width();
    // NW convention: v = 0 at the projected rectangle's north edge.
    const double expectedV =
        (expectedProjectedRectangle.north() - projectedInterior.y()) /
        expectedProjectedRectangle.height();
    EXPECT_NEAR(expectedU,
                primitive.vertexTexCoords[0][interiorVertexIndex][0],
                1e-6);
    EXPECT_NEAR(expectedV,
                primitive.vertexTexCoords[0][interiorVertexIndex][1],
                1e-6);
    EXPECT_GT(
        std::abs(primitive.vertexTexCoords[0][interiorVertexIndex][1] -
                 primitive.vertices[interiorVertexIndex].uv[1]),
        1e-3);

    ASSERT_TRUE(result.metadata.rasterOverlayDetails.has_value());
    const Rectangle* metadataRasterRectangle =
        result.metadata.rasterOverlayDetails
            ->findRectangleForOverlayProjection(
                RasterOverlayProjection::WebMercator);
    ASSERT_NE(nullptr, metadataRasterRectangle);
    EXPECT_TRUE(metadataRasterRectangle->equalsEpsilon(
        expectedProjectedRectangle,
        1e-6));
    EXPECT_EQ(tileRectangle,
              result.metadata.rasterOverlayDetails->boundingRegion.rectangle);
}

TEST(QuantizedMeshContentLoaderTest,
     PlacesEastSkirtOutsideTileLikeCesiumNative) {
    const std::vector<uint8_t> bytes = makeQuadQuantizedMeshBytesWithEdges();
    const Rectangle tileRectangle = geographicRootWestRectangle();

    QuantizedMeshContentLoadResult result =
        QuantizedMeshContentLoader::load(
            bytes.data(),
            bytes.size(),
            tileRectangle,
            false,
            {});

    ASSERT_TRUE(result.success());
    ASSERT_NE(nullptr, result.gltfModel);
    ASSERT_EQ(1u, result.gltfModel->primitives.size());
    const GltfPrimitive& primitive = result.gltfModel->primitives.front();
    ASSERT_TRUE(primitive.skirtMetadata.has_value());
    ASSERT_EQ(4u, primitive.skirtMetadata->noSkirtVerticesCount);
    ASSERT_GE(primitive.vertices.size(), 10u);

    const size_t eastSkirtBegin =
        primitive.skirtMetadata->noSkirtVerticesCount + 2u + 2u;
    const Ellipsoid& ellipsoid = Ellipsoid::WGS84();
    const double expectedLongitude =
        tileRectangle.east() + tileRectangle.width() * 0.0001;

    for (size_t i = 0; i < 2u; ++i) {
        const Cartographic cartographic = ellipsoid.cartesianToCartographic(
            primitive.vertices[eastSkirtBegin + i].positionEcef);
        EXPECT_GT(cartographic.longitude(), tileRectangle.east());
        EXPECT_NEAR(expectedLongitude, cartographic.longitude(), 1e-10);
    }
}

TEST(QuantizedMeshContentLoaderTest,
     CarriesTerrainSidecarMetadataWithoutSurfaceMeshDependency) {
    const Vec3 horizonOcclusionPoint(0.25, -0.5, 0.75);
    const std::vector<uint8_t> bytes =
        makeQuantizedMeshBytes(-20.0f, 320.0f, horizonOcclusionPoint);

    QuantizedMeshContentLoadResult result =
        QuantizedMeshContentLoader::load(
            bytes.data(),
            bytes.size(),
            geographicRootWestRectangle(),
            false,
            {});

    ASSERT_TRUE(result.success());
    ASSERT_TRUE(result.metadata.updatedBoundingVolume.has_value());
    EXPECT_NEAR(-20.0,
                result.metadata.updatedBoundingVolume->minimumHeight,
                1e-6);
    EXPECT_NEAR(320.0,
                result.metadata.updatedBoundingVolume->maximumHeight,
                1e-6);
    ASSERT_TRUE(result.metadata.terrainHeightRange.has_value());
    EXPECT_NEAR(-20.0, result.metadata.terrainHeightRange->first, 1e-6);
    EXPECT_NEAR(320.0, result.metadata.terrainHeightRange->second, 1e-6);
    ASSERT_TRUE(result.metadata.horizonOcclusionPoint.has_value());
    EXPECT_LT((*result.metadata.horizonOcclusionPoint -
               horizonOcclusionPoint)
                  .length(),
              1e-12);
}

TEST(QuantizedMeshContentLoaderTest,
     GeneratesWebMercatorTexCoordsFromWorldPositionsAfterRtcTransform) {
    const Rectangle tileRectangle = geographicRootWestRectangle();
    const Cartographic center = Cartographic::fromRadians(
        tileRectangle.west() + tileRectangle.width() * 0.5,
        tileRectangle.south() + tileRectangle.height() * 0.5,
        0.0);
    const Vec3 localOrigin =
        Ellipsoid::WGS84().cartographicToCartesian(center);
    const std::vector<uint8_t> bytes =
        makeQuantizedMeshBytesWithInteriorVertex(localOrigin);

    QuantizedMeshContentLoadResult result =
        QuantizedMeshContentLoader::load(
            bytes.data(),
            bytes.size(),
            tileRectangle,
            false,
            {},
            RasterOverlayProjection::WebMercator,
            std::nullopt,
            QuantizedMeshContentLoader::RasterOverlayDetailsMode::
                GenerateTerrainProjection);

    ASSERT_TRUE(result.success());
    ASSERT_NE(nullptr, result.gltfModel);
    ASSERT_EQ(1u, result.gltfModel->primitives.size());
    ASSERT_EQ(1u, result.gltfModel->nodes.size());

    const GltfPrimitive& primitive = result.gltfModel->primitives.front();
    ASSERT_EQ(primitive.vertices.size(), primitive.vertexTexCoords[0].size());
    ASSERT_GT(primitive.vertices.size(), 3u);
    ASSERT_GE(primitive.runtime.nodeIndex, 0);

    const size_t interiorVertexIndex = 3u;
    const Mat4& nodeTransform =
        result.gltfModel->nodes[static_cast<size_t>(
            primitive.runtime.nodeIndex)]
            .globalTransform;
    const Vec3 worldPosition = nodeTransform.transformPoint(
        primitive.vertices[interiorVertexIndex].positionEcef);

    const WebMercatorProjection projection(Ellipsoid::WGS84());
    const Rectangle expectedProjectedRectangle = projectRectangleSimple(
        projection,
        tileRectangle);
    const Cartographic interiorCartographic =
        Ellipsoid::WGS84().cartesianToCartographic(worldPosition);
    const Vec3 projectedInterior =
        projectPosition(projection, interiorCartographic);
    const double expectedU =
        (projectedInterior.x() - expectedProjectedRectangle.west()) /
        expectedProjectedRectangle.width();
    // NW convention: v = 0 at the projected rectangle's north edge.
    const double expectedV =
        (expectedProjectedRectangle.north() - projectedInterior.y()) /
        expectedProjectedRectangle.height();

    EXPECT_NEAR(expectedU,
                primitive.vertexTexCoords[0][interiorVertexIndex][0],
                1e-6);
    EXPECT_NEAR(expectedV,
                primitive.vertexTexCoords[0][interiorVertexIndex][1],
                1e-6);

    const Vec3 localPosition =
        primitive.vertices[interiorVertexIndex].positionEcef;
    const Cartographic localCartographic =
        Ellipsoid::WGS84().cartesianToCartographic(localPosition);
    const Vec3 incorrectlyProjectedLocal =
        projectPosition(projection, localCartographic);
    // Same NW convention as expectedV, but derived from the (wrong) local
    // position; the actual texcoord must differ measurably from it.
    const double wrongV =
        (expectedProjectedRectangle.north() - incorrectlyProjectedLocal.y()) /
        expectedProjectedRectangle.height();
    EXPECT_GT(std::abs(wrongV -
                       primitive.vertexTexCoords[0][interiorVertexIndex][1]),
              1e-3);
}

TEST(QuantizedMeshContentLoaderTest,
     CarriesMetadataAvailabilityUpdatesLikeLayerJsonTerrainLoader) {
    const std::string metadataJson = R"json({
        "available": [
            [
                {"startX": 0, "startY": 0, "endX": 1, "endY": 1}
            ],
            [
                {"startX": 1, "startY": 2, "endX": 3, "endY": 4}
            ]
        ]
    })json";
    const std::vector<uint8_t> bytes =
        makeQuantizedMeshBytesWithMetadata(metadataJson);
    const TileKey subtreeKey{"Geographic-TMS", 2, 1, 1};
    std::vector<QuantizedMeshMetadataContent> metadata;
    metadata.push_back(QuantizedMeshMetadataContent{
        3,
        subtreeKey,
        bytes.data(),
        bytes.size()});

    QuantizedMeshContentLoadResult result =
        QuantizedMeshContentLoader::load(
            bytes.data(),
            bytes.size(),
            geographicRootWestRectangle(),
            false,
            metadata);

    EXPECT_TRUE(result.success());
    ASSERT_EQ(1u, result.availabilityUpdates.size());
    const QuantizedMeshAvailabilityUpdate& update =
        result.availabilityUpdates.front();
    EXPECT_EQ(3, update.layerIndex);
    EXPECT_EQ(subtreeKey, update.subtreeKey);
    ASSERT_EQ(2u, update.metadataAvailability.size());
    EXPECT_EQ(0u, update.metadataAvailability[0][0]);
    EXPECT_EQ(0u, update.metadataAvailability[0][1]);
    EXPECT_EQ(1u, update.metadataAvailability[0][4]);
    EXPECT_EQ(1u, update.metadataAvailability[1][0]);
    EXPECT_EQ(1u, update.metadataAvailability[1][1]);
    EXPECT_EQ(4u, update.metadataAvailability[1][4]);
}

TEST(QuantizedMeshContentLoaderTest,
     CarriesCurrentTileMetadataFromDecodedContentLikeCesiumNative) {
    const std::string metadataJson = R"json({
        "available": [
            [
                {"startX": 2, "startY": 0, "endX": 3, "endY": 1}
            ],
            [
                {"startX": 4, "startY": 2, "endX": 7, "endY": 3}
            ]
        ]
    })json";
    const std::vector<uint8_t> bytes =
        makeQuantizedMeshBytesWithMetadata(metadataJson);
    const TileKey subtreeKey{"Geographic-TMS", 3, 2, 1};
    QuantizedMeshAvailabilityUpdate currentTileUpdate;
    currentTileUpdate.layerIndex = 5;
    currentTileUpdate.subtreeKey = subtreeKey;

    QuantizedMeshContentLoadResult result =
        QuantizedMeshContentLoader::load(
            bytes.data(),
            bytes.size(),
            geographicRootWestRectangle(),
            false,
            {},
            RasterOverlayProjection::Geographic,
            currentTileUpdate);

    EXPECT_TRUE(result.success());
    ASSERT_EQ(1u, result.availabilityUpdates.size());
    const QuantizedMeshAvailabilityUpdate& update =
        result.availabilityUpdates.front();
    EXPECT_EQ(5, update.layerIndex);
    EXPECT_EQ(subtreeKey, update.subtreeKey);
    ASSERT_EQ(2u, update.metadataAvailability.size());
    EXPECT_EQ((QuantizedMeshAvailabilityRange{0, 2, 0, 3, 1}),
              update.metadataAvailability[0]);
    EXPECT_EQ((QuantizedMeshAvailabilityRange{1, 4, 2, 7, 3}),
              update.metadataAvailability[1]);
}

TEST(QuantizedMeshContentLoaderTest,
     MalformedCurrentTileMetadataKeepsContentRenderableWithDiagnostic) {
    const std::vector<uint8_t> bytes =
        makeQuantizedMeshBytesWithMetadata("{");
    const TileKey subtreeKey{"Geographic-TMS", 3, 2, 1};
    QuantizedMeshAvailabilityUpdate currentTileUpdate;
    currentTileUpdate.layerIndex = 5;
    currentTileUpdate.subtreeKey = subtreeKey;

    QuantizedMeshContentLoadResult result =
        QuantizedMeshContentLoader::load(
            bytes.data(),
            bytes.size(),
            geographicRootWestRectangle(),
            false,
            {},
            RasterOverlayProjection::Geographic,
            currentTileUpdate);

    EXPECT_TRUE(result.success());
    ASSERT_EQ(1u, result.availabilityUpdates.size());
    EXPECT_TRUE(
        result.availabilityUpdates.front().metadataAvailability.empty());
    ASSERT_EQ(1u, result.diagnostics.size());
    EXPECT_NE(std::string::npos,
              result.diagnostics.front().find("Error when parsing metadata"));
}

TEST(QuantizedMeshContentLoaderTest,
     MalformedExternalMetadataKeepsContentRenderableWithDiagnostic) {
    const std::vector<uint8_t> bytes = makeQuantizedMeshBytes();
    const std::vector<uint8_t> malformedMetadata =
        makeQuantizedMeshBytesWithMetadata("{");
    const TileKey subtreeKey{"Geographic-TMS", 2, 1, 1};
    std::vector<QuantizedMeshMetadataContent> metadata;
    metadata.push_back(QuantizedMeshMetadataContent{
        3,
        subtreeKey,
        malformedMetadata.data(),
        malformedMetadata.size()});

    QuantizedMeshContentLoadResult result =
        QuantizedMeshContentLoader::load(
            bytes.data(),
            bytes.size(),
            geographicRootWestRectangle(),
            false,
            metadata);

    EXPECT_TRUE(result.success());
    ASSERT_EQ(1u, result.availabilityUpdates.size());
    EXPECT_EQ(3, result.availabilityUpdates.front().layerIndex);
    EXPECT_EQ(subtreeKey, result.availabilityUpdates.front().subtreeKey);
    EXPECT_TRUE(
        result.availabilityUpdates.front().metadataAvailability.empty());
    ASSERT_EQ(1u, result.diagnostics.size());
    EXPECT_NE(std::string::npos,
              result.diagnostics.front().find("Error when parsing metadata"));
}

TEST(QuantizedMeshContentLoaderTest,
     AcceptsCompleteHighTriangleCountMeshLikeCesiumNative) {
    const std::vector<uint8_t> bytes =
        makeHighTriangleCountQuantizedMeshBytes();

    QuantizedMeshContentLoadResult result =
        QuantizedMeshContentLoader::load(
            bytes.data(),
            bytes.size(),
            geographicRootWestRectangle(),
            false,
            {});

    ASSERT_TRUE(result.success());
    ASSERT_NE(nullptr, result.gltfModel);
    ASSERT_EQ(1u, result.gltfModel->primitives.size());
    EXPECT_GE(result.gltfModel->primitives.front().indices.size(), 39u);
    EXPECT_TRUE(result.availabilityUpdates.empty());
}

TEST(QuantizedMeshContentLoaderTest,
     InvalidBodyPreservesKnownAvailabilityUpdatesLikeLayerJsonTerrainLoader) {
    const uint8_t invalid[] = {0, 1, 2, 3};
    QuantizedMeshAvailabilityUpdate currentTileUpdate;
    currentTileUpdate.layerIndex = 2;
    currentTileUpdate.subtreeKey = TileKey{"Geographic-TMS", 1, 0, 0};

    QuantizedMeshContentLoadResult result =
        QuantizedMeshContentLoader::load(
            invalid,
            sizeof(invalid),
            geographicRootWestRectangle(),
            false,
            {QuantizedMeshMetadataContent{
                0,
                TileKey{"Geographic-TMS", 0, 0, 0},
                invalid,
                sizeof(invalid)}},
            RasterOverlayProjection::Geographic,
            currentTileUpdate);

    EXPECT_FALSE(result.success());
    EXPECT_EQ(QuantizedMeshContentLoadStatus::Failed, result.status);
    EXPECT_EQ(nullptr, result.gltfModel);
    EXPECT_FALSE(result.metadata.updatedBoundingVolume.has_value());
    EXPECT_FALSE(result.metadata.updatedContentBoundingVolume.has_value());
    EXPECT_FALSE(result.metadata.rasterOverlayDetails.has_value());
    ASSERT_EQ(2u, result.availabilityUpdates.size());
    EXPECT_EQ(2, result.availabilityUpdates[0].layerIndex);
    EXPECT_EQ(
        currentTileUpdate.subtreeKey,
        result.availabilityUpdates[0].subtreeKey);
    EXPECT_EQ(0, result.availabilityUpdates[1].layerIndex);
    EXPECT_EQ((TileKey{"Geographic-TMS", 0, 0, 0}),
              result.availabilityUpdates[1].subtreeKey);

    TileContentLoadResult contentResult =
        QuantizedMeshContentLoader::toTileContentLoadResult(std::move(result));
    EXPECT_EQ(TileLoadStatus::Failed, contentResult.status);
    ASSERT_EQ(2u, contentResult.quantizedMeshAvailabilityUpdates.size());
}
