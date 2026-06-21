#include <gtest/gtest.h>

#include "earth_engine/terrain/QuantizedMeshParser.h"
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

} // namespace

TEST(QuantizedMeshParserWaterMaskTest,
     OneByteMasksMatchCesiumNativeOnlyLandOnlyWaterSemantics) {
    const std::vector<uint8_t> allWaterBytes =
        makeQuantizedMeshBytesWithWaterMask({255});
    std::unique_ptr<SurfaceTileMesh> allWater =
        QuantizedMeshParser::parseToSurfaceTileMesh(
            allWaterBytes.data(),
            allWaterBytes.size(),
            rootRectangle(),
            true);

    ASSERT_NE(nullptr, allWater);
    EXPECT_TRUE(allWater->waterMask.allWater);
    EXPECT_FALSE(allWater->waterMask.allLand);
    EXPECT_TRUE(allWater->waterMask.data.empty());

    const std::vector<uint8_t> allLandBytes =
        makeQuantizedMeshBytesWithWaterMask({0});
    std::unique_ptr<SurfaceTileMesh> allLand =
        QuantizedMeshParser::parseToSurfaceTileMesh(
            allLandBytes.data(),
            allLandBytes.size(),
            rootRectangle(),
            true);

    ASSERT_NE(nullptr, allLand);
    EXPECT_TRUE(allLand->waterMask.allLand);
    EXPECT_FALSE(allLand->waterMask.allWater);
    EXPECT_TRUE(allLand->waterMask.data.empty());
}

TEST(QuantizedMeshParserWaterMaskTest,
     FullMaskPreservesCesiumNativeWaterAlphaSemanticsForRenderer) {
    std::vector<uint8_t> mask(256 * 256, 0);
    mask[0] = 7;
    mask[12345] = 255;
    const std::vector<uint8_t> bytes =
        makeQuantizedMeshBytesWithWaterMask(mask);

    std::unique_ptr<SurfaceTileMesh> mesh =
        QuantizedMeshParser::parseToSurfaceTileMesh(
            bytes.data(),
            bytes.size(),
            rootRectangle(),
            true);

    ASSERT_NE(nullptr, mesh);
    EXPECT_FALSE(mesh->waterMask.allLand);
    EXPECT_FALSE(mesh->waterMask.allWater);
    ASSERT_EQ(256u * 256u * 4u, mesh->waterMask.data.size());
    EXPECT_EQ(7, mesh->waterMask.data[3]);
    EXPECT_EQ(255, mesh->waterMask.data[12345 * 4]);
    EXPECT_EQ(255, mesh->waterMask.data[12345 * 4 + 1]);
    EXPECT_EQ(255, mesh->waterMask.data[12345 * 4 + 2]);
    EXPECT_EQ(255, mesh->waterMask.data[12345 * 4 + 3]);
}

TEST(QuantizedMeshParserWaterMaskTest,
     LaterWaterMaskExtensionReplacesEarlierOneLikeCesiumNative) {
    std::vector<uint8_t> mask(256 * 256, 0);
    mask[42] = 255;
    std::vector<uint8_t> bytes = makeQuantizedMeshBytesWithWaterMask(mask);
    appendPod<uint8_t>(bytes, 2);
    appendPod<uint32_t>(bytes, 1);
    appendPod<uint8_t>(bytes, 255);

    std::unique_ptr<SurfaceTileMesh> mesh =
        QuantizedMeshParser::parseToSurfaceTileMesh(
            bytes.data(),
            bytes.size(),
            rootRectangle(),
            true);

    ASSERT_NE(nullptr, mesh);
    EXPECT_TRUE(mesh->waterMask.allWater);
    EXPECT_FALSE(mesh->waterMask.allLand);
    EXPECT_TRUE(mesh->waterMask.data.empty());
}

TEST(QuantizedMeshParserWaterMaskTest,
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

    std::unique_ptr<SurfaceTileMesh> invalidWater =
        QuantizedMeshParser::parseToSurfaceTileMesh(
            invalidWaterThenMetadata.data(),
            invalidWaterThenMetadata.size(),
            rootRectangle(),
            true);

    ASSERT_NE(nullptr, invalidWater);
    EXPECT_TRUE(invalidWater->waterMask.allLand);
    EXPECT_FALSE(invalidWater->waterMask.allWater);
    EXPECT_TRUE(invalidWater->waterMask.data.empty());
    EXPECT_EQ((std::vector<QuantizedMeshAvailabilityRange>{
                  {0, 0, 0, 1, 0}}),
              invalidWater->metadataAvailability);

    std::vector<uint8_t> unknownThenWaterBytes = makeQuantizedMeshBytes();
    appendPod<uint8_t>(unknownThenWaterBytes, 99);
    appendPod<uint32_t>(unknownThenWaterBytes, 3);
    appendPod<uint8_t>(unknownThenWaterBytes, 11);
    appendPod<uint8_t>(unknownThenWaterBytes, 22);
    appendPod<uint8_t>(unknownThenWaterBytes, 33);
    appendPod<uint8_t>(unknownThenWaterBytes, 2);
    appendPod<uint32_t>(unknownThenWaterBytes, 1);
    appendPod<uint8_t>(unknownThenWaterBytes, 255);

    std::unique_ptr<SurfaceTileMesh> unknownThenWater =
        QuantizedMeshParser::parseToSurfaceTileMesh(
            unknownThenWaterBytes.data(),
            unknownThenWaterBytes.size(),
            rootRectangle(),
            true);

    ASSERT_NE(nullptr, unknownThenWater);
    EXPECT_TRUE(unknownThenWater->waterMask.allWater);
    EXPECT_FALSE(unknownThenWater->waterMask.allLand);
}

TEST(QuantizedMeshParserWaterMaskTest,
     DisabledWaterMaskStillAdvancesToLaterMetadataLikeCesiumNative) {
    const std::vector<uint8_t> allWaterBytes =
        makeQuantizedMeshBytesWithWaterMask({255});
    std::unique_ptr<SurfaceTileMesh> disabled =
        QuantizedMeshParser::parseToSurfaceTileMesh(
            allWaterBytes.data(),
            allWaterBytes.size(),
            rootRectangle(),
            false);

    ASSERT_NE(nullptr, disabled);
    EXPECT_FALSE(disabled->waterMask.allWater);
    EXPECT_TRUE(disabled->waterMask.allLand);
    EXPECT_TRUE(disabled->waterMask.data.empty());
    EXPECT_FALSE(disabled->waterMask.valid());

    const std::string metadata = R"json({
      "available": [
        [{"startX":0,"startY":0,"endX":1,"endY":0}]
      ]
    })json";
    std::vector<uint8_t> disabledWaterThenMetadata =
        makeQuantizedMeshBytesWithWaterMask({255});
    appendMetadataExtension(disabledWaterThenMetadata, metadata);

    std::unique_ptr<SurfaceTileMesh> disabledWithMetadata =
        QuantizedMeshParser::parseToSurfaceTileMesh(
            disabledWaterThenMetadata.data(),
            disabledWaterThenMetadata.size(),
            rootRectangle(),
            false);

    ASSERT_NE(nullptr, disabledWithMetadata);
    EXPECT_FALSE(disabledWithMetadata->waterMask.valid());
    EXPECT_EQ((std::vector<QuantizedMeshAvailabilityRange>{
                  {0, 0, 0, 1, 0}}),
              disabledWithMetadata->metadataAvailability);
}
