#include <gtest/gtest.h>

#include "earth_engine/content/QuantizedMeshContentLoader.h"
#include "earth_engine/tiling/TileKey.h"
#include "earth_engine/tiling/TileScheme.h"

#include <cstdint>
#include <memory>
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

void appendMetadataExtension(std::vector<uint8_t>& bytes,
                             const std::string& metadataJson) {
    appendPod<uint8_t>(bytes, 4);
    appendPod<uint32_t>(
        bytes,
        static_cast<uint32_t>(sizeof(uint32_t) + metadataJson.size()));
    appendPod<uint32_t>(bytes, static_cast<uint32_t>(metadataJson.size()));
    bytes.insert(bytes.end(), metadataJson.begin(), metadataJson.end());
}

std::vector<uint8_t> makeQuantizedMeshBytes() {
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

    appendPod<uint32_t>(bytes, 1);
    for (int i = 0; i < 3; ++i) appendPod<uint16_t>(bytes, 0);
    for (int i = 0; i < 4; ++i) appendPod<uint32_t>(bytes, 0);
    return bytes;
}

std::vector<uint8_t> makeQuantizedMeshBytesWithWaterMask(
    const std::vector<uint8_t>& waterMask) {
    std::vector<uint8_t> bytes = makeQuantizedMeshBytes();
    appendPod<uint8_t>(bytes, 2);
    appendPod<uint32_t>(bytes, static_cast<uint32_t>(waterMask.size()));
    bytes.insert(bytes.end(), waterMask.begin(), waterMask.end());
    return bytes;
}

Rectangle rootRectangle() {
    auto scheme = TileScheme::createGeographicTMS();
    return scheme->tileToRectangle(TileKey{"Geographic-TMS", 0, 0, 0});
}

QuantizedMeshContentLoadResult loadQuantizedMeshContent(
    const std::vector<uint8_t>& bytes,
    bool enableWaterMask,
    bool includeCurrentLayerMetadata = false) {
    std::vector<QuantizedMeshMetadataContent> metadata;
    if (includeCurrentLayerMetadata) {
        QuantizedMeshMetadataContent currentLayerMetadata;
        currentLayerMetadata.layerIndex = 0;
        currentLayerMetadata.subtreeKey = TileKey{"Geographic-TMS", 0, 0, 0};
        currentLayerMetadata.data = bytes.data();
        currentLayerMetadata.size = bytes.size();
        metadata.push_back(currentLayerMetadata);
    }
    return QuantizedMeshContentLoader::load(
        bytes.data(),
        bytes.size(),
        rootRectangle(),
        enableWaterMask,
        metadata);
}

} // namespace

TEST(QuantizedMeshContentLoaderWaterMaskTest,
     OneByteMasksMatchCesiumNativeOnlyLandOnlyWaterSemantics) {
    const std::vector<uint8_t> allWaterBytes =
        makeQuantizedMeshBytesWithWaterMask({255});
    QuantizedMeshContentLoadResult allWater =
        loadQuantizedMeshContent(allWaterBytes, true);

    ASSERT_TRUE(allWater.success());
    EXPECT_TRUE(allWater.gltfModel->terrainWaterMask.allWater);
    EXPECT_FALSE(allWater.gltfModel->terrainWaterMask.allLand);
    EXPECT_TRUE(allWater.gltfModel->terrainWaterMask.data.empty());
    ASSERT_EQ(1u, allWater.gltfModel->primitives.size());
    EXPECT_TRUE(
        allWater.gltfModel->primitives.front().hasTerrainWaterMaskMetadata);
    EXPECT_TRUE(allWater.gltfModel->primitives.front().terrainOnlyWater);
    EXPECT_FALSE(allWater.gltfModel->primitives.front().terrainOnlyLand);
    EXPECT_FALSE(
        allWater.gltfModel->primitives.front()
            .terrainWaterMaskTextureIndex.has_value());

    const std::vector<uint8_t> allLandBytes =
        makeQuantizedMeshBytesWithWaterMask({0});
    QuantizedMeshContentLoadResult allLand =
        loadQuantizedMeshContent(allLandBytes, true);

    ASSERT_TRUE(allLand.success());
    EXPECT_TRUE(allLand.gltfModel->terrainWaterMask.allLand);
    EXPECT_FALSE(allLand.gltfModel->terrainWaterMask.allWater);
    EXPECT_TRUE(allLand.gltfModel->terrainWaterMask.data.empty());
    ASSERT_EQ(1u, allLand.gltfModel->primitives.size());
    EXPECT_TRUE(
        allLand.gltfModel->primitives.front().hasTerrainWaterMaskMetadata);
    EXPECT_FALSE(allLand.gltfModel->primitives.front().terrainOnlyWater);
    EXPECT_TRUE(allLand.gltfModel->primitives.front().terrainOnlyLand);
    EXPECT_FALSE(
        allLand.gltfModel->primitives.front()
            .terrainWaterMaskTextureIndex.has_value());
}

TEST(QuantizedMeshContentLoaderWaterMaskTest,
     FullMaskPreservesCesiumNativeSingleChannelWaterSemanticsForRenderer) {
    std::vector<uint8_t> mask(256 * 256, 0);
    mask[0] = 7;
    mask[12345] = 255;
    const std::vector<uint8_t> bytes =
        makeQuantizedMeshBytesWithWaterMask(mask);

    QuantizedMeshContentLoadResult result =
        loadQuantizedMeshContent(bytes, true);

    ASSERT_TRUE(result.success());
    const WaterMask& waterMask = result.gltfModel->terrainWaterMask;
    EXPECT_FALSE(waterMask.allLand);
    EXPECT_FALSE(waterMask.allWater);
    ASSERT_EQ(256u * 256u, waterMask.data.size());
    EXPECT_EQ(7, waterMask.data[0]);
    EXPECT_EQ(255, waterMask.data[12345]);
    ASSERT_TRUE(result.gltfModel->terrainWaterMaskTextureIndex.has_value());
    ASSERT_LT(*result.gltfModel->terrainWaterMaskTextureIndex,
              result.gltfModel->textures.size());
    ASSERT_EQ(1u, result.gltfModel->primitives.size());
    const GltfPrimitive& primitive = result.gltfModel->primitives.front();
    EXPECT_TRUE(primitive.hasTerrainWaterMaskMetadata);
    EXPECT_FALSE(primitive.terrainOnlyWater);
    EXPECT_FALSE(primitive.terrainOnlyLand);
    ASSERT_TRUE(primitive.terrainWaterMaskTextureIndex.has_value());
    EXPECT_EQ(*result.gltfModel->terrainWaterMaskTextureIndex,
              *primitive.terrainWaterMaskTextureIndex);
    EXPECT_DOUBLE_EQ(0.0, primitive.terrainWaterMaskTranslationX);
    EXPECT_DOUBLE_EQ(0.0, primitive.terrainWaterMaskTranslationY);
    EXPECT_DOUBLE_EQ(1.0, primitive.terrainWaterMaskScale);
    const GltfTexture& texture =
        result.gltfModel->textures[*result.gltfModel->terrainWaterMaskTextureIndex];
    EXPECT_EQ(256, texture.image.width);
    EXPECT_EQ(256, texture.image.height);
    EXPECT_EQ(1, texture.image.channels);
    ASSERT_EQ(256u * 256u, texture.image.pixels.size());
    EXPECT_EQ(7, texture.image.pixels[0]);
    EXPECT_EQ(255, texture.image.pixels[12345]);
}

TEST(QuantizedMeshContentLoaderWaterMaskTest,
     LaterWaterMaskExtensionReplacesEarlierOneLikeCesiumNative) {
    std::vector<uint8_t> mask(256 * 256, 0);
    mask[42] = 255;
    std::vector<uint8_t> bytes = makeQuantizedMeshBytesWithWaterMask(mask);
    appendPod<uint8_t>(bytes, 2);
    appendPod<uint32_t>(bytes, 1);
    appendPod<uint8_t>(bytes, 255);

    QuantizedMeshContentLoadResult result =
        loadQuantizedMeshContent(bytes, true);

    ASSERT_TRUE(result.success());
    EXPECT_TRUE(result.gltfModel->terrainWaterMask.allWater);
    EXPECT_FALSE(result.gltfModel->terrainWaterMask.allLand);
    EXPECT_TRUE(result.gltfModel->terrainWaterMask.data.empty());
}

TEST(QuantizedMeshContentLoaderWaterMaskTest,
     UnsupportedAndUnknownExtensionsAdvanceToLaterKnownExtensions) {
    const std::string metadata = R"json({
      "available": [
        [{"startX":0,"startY":0,"endX":1,"endY":0}]
      ]
    })json";

    std::vector<uint8_t> invalidWaterThenMetadata = makeQuantizedMeshBytes();
    appendPod<uint8_t>(invalidWaterThenMetadata, 2);
    appendPod<uint32_t>(invalidWaterThenMetadata, 2);
    appendPod<uint8_t>(invalidWaterThenMetadata, 7);
    appendPod<uint8_t>(invalidWaterThenMetadata, 9);
    appendMetadataExtension(invalidWaterThenMetadata, metadata);

    QuantizedMeshContentLoadResult invalidWater =
        loadQuantizedMeshContent(invalidWaterThenMetadata, true, true);

    ASSERT_TRUE(invalidWater.success());
    EXPECT_TRUE(invalidWater.gltfModel->terrainWaterMask.allLand);
    EXPECT_FALSE(invalidWater.gltfModel->terrainWaterMask.allWater);
    EXPECT_TRUE(invalidWater.gltfModel->terrainWaterMask.data.empty());
    ASSERT_EQ(1u, invalidWater.availabilityUpdates.size());
    EXPECT_EQ((std::vector<QuantizedMeshAvailabilityRange>{
                  {0, 0, 0, 1, 0}}),
              invalidWater.availabilityUpdates.front().metadataAvailability);

    std::vector<uint8_t> unknownThenWaterBytes = makeQuantizedMeshBytes();
    appendPod<uint8_t>(unknownThenWaterBytes, 99);
    appendPod<uint32_t>(unknownThenWaterBytes, 3);
    appendPod<uint8_t>(unknownThenWaterBytes, 11);
    appendPod<uint8_t>(unknownThenWaterBytes, 22);
    appendPod<uint8_t>(unknownThenWaterBytes, 33);
    appendPod<uint8_t>(unknownThenWaterBytes, 2);
    appendPod<uint32_t>(unknownThenWaterBytes, 1);
    appendPod<uint8_t>(unknownThenWaterBytes, 255);

    QuantizedMeshContentLoadResult unknownThenWater =
        loadQuantizedMeshContent(unknownThenWaterBytes, true);

    ASSERT_TRUE(unknownThenWater.success());
    EXPECT_TRUE(unknownThenWater.gltfModel->terrainWaterMask.allWater);
    EXPECT_FALSE(unknownThenWater.gltfModel->terrainWaterMask.allLand);
}

TEST(QuantizedMeshContentLoaderWaterMaskTest,
     DisabledWaterMaskStillAdvancesToLaterMetadataLikeCesiumNative) {
    const std::vector<uint8_t> allWaterBytes =
        makeQuantizedMeshBytesWithWaterMask({255});
    QuantizedMeshContentLoadResult disabled =
        loadQuantizedMeshContent(allWaterBytes, false);

    ASSERT_TRUE(disabled.success());
    EXPECT_FALSE(disabled.gltfModel->terrainWaterMask.allWater);
    EXPECT_TRUE(disabled.gltfModel->terrainWaterMask.allLand);
    EXPECT_TRUE(disabled.gltfModel->terrainWaterMask.data.empty());
    EXPECT_FALSE(disabled.gltfModel->terrainWaterMask.valid());

    const std::string metadata = R"json({
      "available": [
        [{"startX":0,"startY":0,"endX":1,"endY":0}]
      ]
    })json";
    std::vector<uint8_t> disabledWaterThenMetadata =
        makeQuantizedMeshBytesWithWaterMask({255});
    appendMetadataExtension(disabledWaterThenMetadata, metadata);

    QuantizedMeshContentLoadResult disabledWithMetadata =
        loadQuantizedMeshContent(disabledWaterThenMetadata, false, true);

    ASSERT_TRUE(disabledWithMetadata.success());
    EXPECT_FALSE(disabledWithMetadata.gltfModel->terrainWaterMask.valid());
    ASSERT_EQ(1u, disabledWithMetadata.availabilityUpdates.size());
    EXPECT_EQ((std::vector<QuantizedMeshAvailabilityRange>{
                  {0, 0, 0, 1, 0}}),
              disabledWithMetadata.availabilityUpdates.front()
                  .metadataAvailability);
}
