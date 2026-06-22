#include <gtest/gtest.h>

#include "earth_engine/content/QuantizedMeshContentLoader.h"
#include "earth_engine/terrain/QuantizedMeshParser.h"
#include "earth_engine/tiling/TileKey.h"
#include "earth_engine/tiling/TileScheme.h"

#include <cstddef>
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

std::vector<uint8_t> makeQuantizedMeshBytes(
    const std::string& metadataJson = "") {
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

    if (!metadataJson.empty()) {
        appendMetadataExtension(bytes, metadataJson);
    }
    return bytes;
}

std::vector<uint8_t> makeZeroTriangleQuantizedMeshBytes(
    const std::string& metadataJson) {
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

    appendPod<uint32_t>(bytes, 0);
    for (int i = 0; i < 4; ++i) appendPod<uint32_t>(bytes, 0);
    appendMetadataExtension(bytes, metadataJson);
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

std::vector<uint8_t> makeQuantizedMeshBytesWithHeaderPadding(
    const std::string& metadataJson) {
    std::vector<uint8_t> bytes = makeQuantizedMeshBytes(metadataJson);
    bytes.insert(bytes.begin() + 92, 4, 0);
    return bytes;
}

std::vector<uint8_t> makeQuantizedMeshBytesWithShortOctNormalThenMetadata(
    const std::string& metadataJson) {
    std::vector<uint8_t> bytes = makeQuantizedMeshBytes();
    appendPod<uint8_t>(bytes, 1);
    appendPod<uint32_t>(bytes, 1);
    appendPod<uint8_t>(bytes, 128);
    appendMetadataExtension(bytes, metadataJson);
    return bytes;
}

std::vector<uint8_t> makeQuantizedMeshBytesWithMalformedMetadataThenMetadata(
    const std::string& metadataJson) {
    std::vector<uint8_t> bytes = makeQuantizedMeshBytes();
    appendPod<uint8_t>(bytes, 4);
    appendPod<uint32_t>(bytes, sizeof(uint32_t));
    appendPod<uint32_t>(bytes, 0xffffffffu);
    appendMetadataExtension(bytes, metadataJson);
    return bytes;
}

std::vector<uint8_t> makeQuantizedMeshBytesWithOversizedMetadataExtensionLength(
    const std::string& metadataJson) {
    std::vector<uint8_t> bytes = makeQuantizedMeshBytes();
    appendPod<uint8_t>(bytes, 4);
    appendPod<uint32_t>(
        bytes,
        static_cast<uint32_t>(sizeof(uint32_t) + metadataJson.size() + 1024));
    appendPod<uint32_t>(bytes, static_cast<uint32_t>(metadataJson.size()));
    bytes.insert(bytes.end(), metadataJson.begin(), metadataJson.end());
    return bytes;
}

std::vector<uint8_t> makeQuantizedMeshBytesWithTwoMetadataExtensions(
    const std::string& firstMetadataJson,
    const std::string& secondMetadataJson) {
    std::vector<uint8_t> bytes = makeQuantizedMeshBytes(firstMetadataJson);
    appendMetadataExtension(bytes, secondMetadataJson);
    return bytes;
}

std::vector<uint8_t> makeQuantizedMeshBytesWithMetadataThenEmptyMetadata(
    const std::string& metadataJson) {
    std::vector<uint8_t> bytes = makeQuantizedMeshBytes(metadataJson);
    appendPod<uint8_t>(bytes, 4);
    appendPod<uint32_t>(bytes, sizeof(uint32_t));
    appendPod<uint32_t>(bytes, 0);
    return bytes;
}

Rectangle rootRectangle() {
    auto scheme = TileScheme::createGeographicTMS();
    return scheme->tileToRectangle(TileKey{"Geographic-TMS", 0, 0, 0});
}

QuantizedMeshContentLoadResult loadQuantizedMeshContentWithCurrentMetadata(
    const std::vector<uint8_t>& bytes) {
    QuantizedMeshMetadataContent currentLayerMetadata;
    currentLayerMetadata.layerIndex = 0;
    currentLayerMetadata.subtreeKey = TileKey{"Geographic-TMS", 0, 0, 0};
    currentLayerMetadata.data = bytes.data();
    currentLayerMetadata.size = bytes.size();
    return QuantizedMeshContentLoader::load(
        bytes.data(),
        bytes.size(),
        rootRectangle(),
        false,
        {currentLayerMetadata});
}

std::vector<QuantizedMeshAvailabilityRange> firstAvailabilityUpdate(
    const QuantizedMeshContentLoadResult& result) {
    if (result.availabilityUpdates.empty()) {
        return {};
    }
    return result.availabilityUpdates.front().metadataAvailability;
}

} // namespace

TEST(QuantizedMeshParserMetadataTest,
     ExtensionLengthPrefixMatchesCesiumNative) {
    const std::string metadata = R"json({
      "available": [
        [{"startX":0,"startY":0,"endX":1,"endY":0}],
        [{"startX":2,"startY":1,"endX":3,"endY":1}]
      ]
    })json";
    const std::vector<uint8_t> bytes = makeQuantizedMeshBytes(metadata);

    QuantizedMeshContentLoadResult result =
        loadQuantizedMeshContentWithCurrentMetadata(bytes);

    ASSERT_TRUE(result.success());
    ASSERT_EQ(1u, result.availabilityUpdates.size());
    ASSERT_EQ(2u, result.availabilityUpdates.front().metadataAvailability.size());
    EXPECT_EQ((QuantizedMeshAvailabilityRange{0, 0, 0, 1, 0}),
              result.availabilityUpdates.front().metadataAvailability[0]);
    EXPECT_EQ((QuantizedMeshAvailabilityRange{1, 2, 1, 3, 1}),
              result.availabilityUpdates.front().metadataAvailability[1]);

    EXPECT_EQ(result.availabilityUpdates.front().metadataAvailability,
              QuantizedMeshParser::parseMetadataAvailability(
                  bytes.data(),
                  bytes.size()));
}

TEST(QuantizedMeshParserMetadataTest,
     ZeroTriangleMeshStillDecodesAvailabilityLikeCesiumNative) {
    const std::string metadata = R"json({
      "available": [
        [{"startX":0,"startY":0,"endX":1,"endY":0}]
      ]
    })json";
    const std::vector<uint8_t> bytes =
        makeZeroTriangleQuantizedMeshBytes(metadata);

    QuantizedMeshContentLoadResult result =
        loadQuantizedMeshContentWithCurrentMetadata(bytes);

    ASSERT_TRUE(result.success());
    ASSERT_EQ(1u, result.gltfModel->primitives.size());
    EXPECT_EQ(3u, result.gltfModel->primitives.front().vertices.size());
    EXPECT_TRUE(result.gltfModel->primitives.front().indices.empty());
    EXPECT_EQ((std::vector<QuantizedMeshAvailabilityRange>{
                  {0, 0, 0, 1, 0}}),
              firstAvailabilityUpdate(result));
}

TEST(QuantizedMeshParserMetadataTest,
     AvailabilityFieldsAndShapesMatchCesiumNative) {
    const std::vector<uint8_t> negativeBytes = makeQuantizedMeshBytes(R"json({
      "available": [
        [{"startX":-4,"startY":1,"endX":2,"endY":-8}]
      ]
    })json");
    EXPECT_EQ((std::vector<QuantizedMeshAvailabilityRange>{
                  {0u, 0u, 1u, 2u, 0u}}),
              QuantizedMeshParser::parseMetadataAvailability(
                  negativeBytes.data(),
                  negativeBytes.size()));

    const std::vector<uint8_t> maxUint32Bytes =
        makeQuantizedMeshBytes(R"json({
      "available": [
        [{"startX":4294967295,"startY":0,"endX":4294967295,"endY":0}]
      ]
    })json");
    EXPECT_EQ((std::vector<QuantizedMeshAvailabilityRange>{
                  {0u, 4294967295u, 0u, 4294967295u, 0u}}),
              QuantizedMeshParser::parseMetadataAvailability(
                  maxUint32Bytes.data(),
                  maxUint32Bytes.size()));

    const std::vector<uint8_t> missingAvailableBytes =
        makeQuantizedMeshBytes(R"json({"foo":[]})json");
    EXPECT_TRUE(QuantizedMeshParser::parseMetadataAvailability(
                    missingAvailableBytes.data(),
                    missingAvailableBytes.size())
                    .empty());

    const std::vector<uint8_t> nonArrayAvailableBytes =
        makeQuantizedMeshBytes(R"json({"available":"not-an-array"})json");
    EXPECT_TRUE(QuantizedMeshParser::parseMetadataAvailability(
                    nonArrayAvailableBytes.data(),
                    nonArrayAvailableBytes.size())
                    .empty());
}

TEST(QuantizedMeshParserMetadataTest,
     NonArrayLevelsDoNotAdvanceStartingLevelLikeCesiumNative) {
    const std::vector<uint8_t> bytes = makeQuantizedMeshBytes(R"json({
      "available": [
        "not-a-level-array",
        [{"startX":0,"startY":0,"endX":1,"endY":0}]
      ]
    })json");

    EXPECT_EQ((std::vector<QuantizedMeshAvailabilityRange>{
                  {0, 0, 0, 1, 0}}),
              QuantizedMeshParser::parseMetadataAvailability(
                  bytes.data(),
                  bytes.size()));
}

TEST(QuantizedMeshParserMetadataTest,
     NonObjectRangesAreSkippedAfterAdvancingLevelLikeCesiumNative) {
    const std::vector<uint8_t> bytes = makeQuantizedMeshBytes(R"json({
      "available": [
        [
          "not-a-range-object",
          {"startX":0,"startY":0,"endX":1,"endY":0}
        ],
        [
          17,
          {"startX":2,"startY":1,"endX":3,"endY":1}
        ]
      ]
    })json");

    EXPECT_EQ((std::vector<QuantizedMeshAvailabilityRange>{
                  {0, 0, 0, 1, 0},
                  {1, 2, 1, 3, 1}}),
              QuantizedMeshParser::parseMetadataAvailability(
                  bytes.data(),
                  bytes.size()));
}

TEST(QuantizedMeshParserMetadataTest,
     MetadataOnlyPathSkipsNonMetadataExtensionsLikeCesiumNative) {
    const std::string metadata = R"json({
      "available": [
        [{"startX":0,"startY":0,"endX":1,"endY":0}]
      ]
    })json";
    std::vector<uint8_t> unknownBytes = makeQuantizedMeshBytes();
    appendPod<uint8_t>(unknownBytes, 99);
    appendPod<uint32_t>(unknownBytes, 3);
    appendPod<uint8_t>(unknownBytes, 11);
    appendPod<uint8_t>(unknownBytes, 22);
    appendPod<uint8_t>(unknownBytes, 33);
    appendMetadataExtension(unknownBytes, metadata);

    EXPECT_EQ((std::vector<QuantizedMeshAvailabilityRange>{
                  {0, 0, 0, 1, 0}}),
              QuantizedMeshParser::parseMetadataAvailability(
                  unknownBytes.data(),
                  unknownBytes.size()));

    std::vector<uint8_t> waterMaskBytes =
        makeQuantizedMeshBytesWithWaterMask({255});
    appendMetadataExtension(waterMaskBytes, R"json({
      "available": [
        [{"startX":1,"startY":0,"endX":1,"endY":1}]
      ]
    })json");
    EXPECT_EQ((std::vector<QuantizedMeshAvailabilityRange>{
                  {0, 1, 0, 1, 1}}),
              QuantizedMeshParser::parseMetadataAvailability(
                  waterMaskBytes.data(),
                  waterMaskBytes.size()));
}

TEST(QuantizedMeshParserMetadataTest,
     ExtensionBoundariesAndReplacementMatchCesiumNative) {
    const std::string metadata = R"json({
      "available": [
        [{"startX":0,"startY":0,"endX":1,"endY":0}]
      ]
    })json";

    const std::vector<uint8_t> paddedBytes =
        makeQuantizedMeshBytesWithHeaderPadding(metadata);
    EXPECT_EQ((std::vector<QuantizedMeshAvailabilityRange>{
                  {0, 0, 0, 1, 0}}),
              QuantizedMeshParser::parseMetadataAvailability(
                  paddedBytes.data(),
                  paddedBytes.size()));

    const std::vector<uint8_t> shortNormalBytes =
        makeQuantizedMeshBytesWithShortOctNormalThenMetadata(metadata);
    QuantizedMeshContentLoadResult shortNormalResult =
        loadQuantizedMeshContentWithCurrentMetadata(shortNormalBytes);
    ASSERT_TRUE(shortNormalResult.success());
    EXPECT_EQ((std::vector<QuantizedMeshAvailabilityRange>{
                  {0, 0, 0, 1, 0}}),
              firstAvailabilityUpdate(shortNormalResult));

    const std::vector<uint8_t> malformedBytes =
        makeQuantizedMeshBytesWithMalformedMetadataThenMetadata(metadata);
    EXPECT_TRUE(QuantizedMeshParser::parseMetadataAvailability(
                    malformedBytes.data(),
                    malformedBytes.size())
                    .empty());

    const std::vector<uint8_t> oversizedBytes =
        makeQuantizedMeshBytesWithOversizedMetadataExtensionLength(metadata);
    EXPECT_EQ((std::vector<QuantizedMeshAvailabilityRange>{
                  {0, 0, 0, 1, 0}}),
              QuantizedMeshParser::parseMetadataAvailability(
                  oversizedBytes.data(),
                  oversizedBytes.size()));

    std::vector<uint8_t> incompleteHeaderBytes =
        makeQuantizedMeshBytes(metadata);
    appendPod<uint8_t>(incompleteHeaderBytes, 99);
    appendPod<uint16_t>(incompleteHeaderBytes, 0x1234u);
    EXPECT_EQ((std::vector<QuantizedMeshAvailabilityRange>{
                  {0, 0, 0, 1, 0}}),
              QuantizedMeshParser::parseMetadataAvailability(
                  incompleteHeaderBytes.data(),
                  incompleteHeaderBytes.size()));

    const std::vector<uint8_t> twoMetadataBytes =
        makeQuantizedMeshBytesWithTwoMetadataExtensions(
            R"json({"available":[[{"startX":0,"startY":0,"endX":0,"endY":0}]]})json",
            R"json({"available":[[{"startX":2,"startY":2,"endX":3,"endY":3}]]})json");
    EXPECT_EQ((std::vector<QuantizedMeshAvailabilityRange>{
                  {0, 2, 2, 3, 3}}),
              QuantizedMeshParser::parseMetadataAvailability(
                  twoMetadataBytes.data(),
                  twoMetadataBytes.size()));

    const std::vector<uint8_t> emptyMetadataBytes =
        makeQuantizedMeshBytesWithMetadataThenEmptyMetadata(metadata);
    EXPECT_TRUE(QuantizedMeshParser::parseMetadataAvailability(
                    emptyMetadataBytes.data(),
                    emptyMetadataBytes.size())
                    .empty());
}
