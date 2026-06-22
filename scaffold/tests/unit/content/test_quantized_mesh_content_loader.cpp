#include <gtest/gtest.h>

#include "earth_engine/content/QuantizedMeshContentLoader.h"
#include "earth_engine/core/math/Vec3.h"

#include <cstdint>
#include <string>
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
    const Vec3& horizonOcclusionPoint = Vec3::zero()) {
    std::vector<uint8_t> bytes;

    for (int i = 0; i < 3; ++i) appendPod<double>(bytes, 0.0);
    appendPod<float>(bytes, minimumHeight);
    appendPod<float>(bytes, maximumHeight);
    for (int i = 0; i < 4; ++i) appendPod<double>(bytes, 0.0);
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
    EXPECT_FALSE(texture.sampler.mipmap);
}

TEST(QuantizedMeshContentLoaderTest,
     LoadsGltfTerrainModelWithoutTerrainProvider) {
    const std::vector<uint8_t> bytes =
        makeQuantizedMeshBytes(-10.0f, 150.0f);

    QuantizedMeshContentLoadResult result =
        QuantizedMeshContentLoader::load(
            bytes.data(),
            bytes.size(),
            geographicRootWestRectangle(),
            false,
            {});

    EXPECT_TRUE(result.success());
    ASSERT_NE(nullptr, result.gltfModel);
    ASSERT_EQ(1u, result.gltfModel->primitives.size());
    const GltfPrimitive& primitive = result.gltfModel->primitives.front();
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
    ASSERT_NE(nullptr, result.surfaceMesh);
    ASSERT_EQ(result.surfaceMesh->vertices.size(), primitive.vertices.size());
    ASSERT_EQ(result.surfaceMesh->indices.size(), primitive.indices.size());
    EXPECT_EQ(result.surfaceMesh->indices, primitive.indices);
    EXPECT_EQ(result.surfaceMesh->vertices.front().uv,
              primitive.vertexTexCoords[0].front());
    EXPECT_EQ(result.surfaceMesh->vertices.back().uv,
              primitive.vertexTexCoords[0].back());
    ASSERT_TRUE(primitive.skirtMetadata.has_value());
    EXPECT_EQ(result.surfaceMesh->skirtMeta.noSkirtIndicesBegin,
              primitive.skirtMetadata->noSkirtIndicesBegin);
    EXPECT_EQ(result.surfaceMesh->skirtMeta.noSkirtIndicesCount,
              primitive.skirtMetadata->noSkirtIndicesCount);
    EXPECT_EQ(result.surfaceMesh->skirtMeta.noSkirtVerticesBegin,
              primitive.skirtMetadata->noSkirtVerticesBegin);
    EXPECT_EQ(result.surfaceMesh->skirtMeta.noSkirtVerticesCount,
              primitive.skirtMetadata->noSkirtVerticesCount);
    EXPECT_EQ(result.surfaceMesh->skirtMeta.meshCenter,
              primitive.skirtMetadata->meshCenter);
    EXPECT_GT(primitive.skirtMetadata->skirtWestHeight, 0.0);
    EXPECT_DOUBLE_EQ(primitive.skirtMetadata->skirtWestHeight,
                     primitive.skirtMetadata->skirtSouthHeight);
    EXPECT_DOUBLE_EQ(primitive.skirtMetadata->skirtWestHeight,
                     primitive.skirtMetadata->skirtEastHeight);
    EXPECT_DOUBLE_EQ(primitive.skirtMetadata->skirtWestHeight,
                     primitive.skirtMetadata->skirtNorthHeight);
    EXPECT_FALSE(result.surfaceMesh->vertices.empty());
    EXPECT_FALSE(result.surfaceMesh->indices.empty());
    EXPECT_TRUE(result.surfaceMesh->hasHeightRange);
    EXPECT_NEAR(-10.0, result.surfaceMesh->minimumHeight, 1e-6);
    EXPECT_NEAR(150.0, result.surfaceMesh->maximumHeight, 1e-6);
    EXPECT_EQ(geographicRootWestRectangle(),
              result.surfaceMesh->rasterOverlayDetails.boundingRegion
                  .rectangle);
    EXPECT_NEAR(-10.0,
                result.surfaceMesh->rasterOverlayDetails.boundingRegion
                    .minimumHeight,
                1e-6);
    EXPECT_NEAR(150.0,
                result.surfaceMesh->rasterOverlayDetails.boundingRegion
                    .maximumHeight,
                1e-6);
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
    EXPECT_TRUE(result.availabilityUpdates.empty());
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

TEST(QuantizedMeshContentLoaderTest, InvalidBodyFailsWithoutUpdates) {
    const uint8_t invalid[] = {0, 1, 2, 3};

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
                sizeof(invalid)}});

    EXPECT_FALSE(result.success());
    EXPECT_EQ(QuantizedMeshContentLoadStatus::Failed, result.status);
    EXPECT_EQ(nullptr, result.gltfModel);
    EXPECT_EQ(nullptr, result.surfaceMesh);
    EXPECT_FALSE(result.metadata.updatedBoundingVolume.has_value());
    EXPECT_FALSE(result.metadata.rasterOverlayDetails.has_value());
    EXPECT_TRUE(result.availabilityUpdates.empty());
}
