#include <gtest/gtest.h>

#include "earth_engine/core/geodesy/Cartographic.h"
#include "earth_engine/core/geodesy/Ellipsoid.h"
#include "earth_engine/terrain/QuantizedMeshParser.h"
#include "earth_engine/tiling/TileKey.h"
#include "earth_engine/tiling/TileScheme.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <utility>
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
    const std::string& metadataJson = "",
    bool includeSkirtEdges = false,
    bool includeOctNormals = false) {
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
    auto appendEdge = [&](std::initializer_list<uint16_t> indices) {
        appendPod<uint32_t>(bytes, static_cast<uint32_t>(indices.size()));
        for (uint16_t index : indices) appendPod<uint16_t>(bytes, index);
    };
    if (includeSkirtEdges) {
        appendEdge({0, 2});
        appendEdge({1, 0});
        appendEdge({1, 2});
        appendEdge({2, 1});
    } else {
        for (int i = 0; i < 4; ++i) appendPod<uint32_t>(bytes, 0);
    }

    if (!metadataJson.empty()) {
        appendMetadataExtension(bytes, metadataJson);
    }
    if (includeOctNormals) {
        appendPod<uint8_t>(bytes, 1);
        appendPod<uint32_t>(bytes, 6);
        const uint8_t normals[] = {
            128, 128,
            255, 128,
            128, 255
        };
        bytes.insert(bytes.end(), normals, normals + sizeof(normals));
    }
    return bytes;
}

std::vector<uint8_t> makeZeroTriangleQuantizedMeshBytes(
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

    appendPod<uint32_t>(bytes, 0);
    for (int i = 0; i < 4; ++i) appendPod<uint32_t>(bytes, 0);
    if (!metadataJson.empty()) {
        appendMetadataExtension(bytes, metadataJson);
    }
    return bytes;
}

std::vector<uint8_t> makeZeroVertexCountQuantizedMeshBytes() {
    std::vector<uint8_t> bytes;
    for (int i = 0; i < 3; ++i) appendPod<double>(bytes, 0.0);
    appendPod<float>(bytes, 0.0f);
    appendPod<float>(bytes, 100.0f);
    appendPod<double>(bytes, 0.0);
    appendPod<double>(bytes, 0.0);
    appendPod<double>(bytes, 0.0);
    appendPod<double>(bytes, 1.0);
    appendPod<double>(bytes, 0.0);
    appendPod<double>(bytes, 0.0);
    appendPod<double>(bytes, 0.0);
    appendPod<uint32_t>(bytes, 0);
    return bytes;
}

std::vector<uint8_t> makeBoundaryUint16QuantizedMeshBytes(
    const std::string& metadataJson = "") {
    constexpr uint32_t vertexCount = 65536u;
    constexpr uint16_t highEdgeIndex = 65535u;

    std::vector<uint8_t> bytes;
    for (int i = 0; i < 3; ++i) appendPod<double>(bytes, 0.0);
    appendPod<float>(bytes, 0.0f);
    appendPod<float>(bytes, 100.0f);
    for (int i = 0; i < 7; ++i) appendPod<double>(bytes, 0.0);
    appendPod<uint32_t>(bytes, vertexCount);

    appendPod<uint16_t>(bytes, zigZagEncode16(0));
    appendPod<uint16_t>(bytes, zigZagEncode16(32767));
    appendPod<uint16_t>(bytes, zigZagEncode16(-32767));
    for (uint32_t i = 3; i < vertexCount; ++i) {
        appendPod<uint16_t>(bytes, zigZagEncode16(0));
    }
    appendPod<uint16_t>(bytes, zigZagEncode16(0));
    appendPod<uint16_t>(bytes, zigZagEncode16(0));
    appendPod<uint16_t>(bytes, zigZagEncode16(32767));
    for (uint32_t i = 3; i < vertexCount; ++i) {
        appendPod<uint16_t>(bytes, zigZagEncode16(0));
    }
    for (uint32_t i = 0; i < vertexCount; ++i) {
        appendPod<uint16_t>(bytes, zigZagEncode16(0));
    }

    appendPod<uint32_t>(bytes, 1);
    appendPod<uint16_t>(bytes, 0);
    appendPod<uint16_t>(bytes, 0);
    appendPod<uint16_t>(bytes, 0);

    auto appendEdge = [&](uint16_t a, uint16_t b) {
        appendPod<uint32_t>(bytes, 2u);
        appendPod<uint16_t>(bytes, a);
        appendPod<uint16_t>(bytes, b);
    };
    appendEdge(0, 2);
    appendEdge(2, highEdgeIndex);
    appendEdge(1, highEdgeIndex);
    appendEdge(0, 1);

    if (!metadataJson.empty()) {
        appendMetadataExtension(bytes, metadataJson);
    }

    return bytes;
}

std::vector<uint8_t> makeLargeQuantizedMeshBytesWithUint32EdgeIndex() {
    constexpr uint32_t vertexCount = 65537u;
    constexpr uint32_t highEdgeIndex = vertexCount - 1u;
    std::vector<uint8_t> bytes;

    for (int i = 0; i < 3; ++i) appendPod<double>(bytes, 0.0);
    appendPod<float>(bytes, 0.0f);
    appendPod<float>(bytes, 100.0f);
    for (int i = 0; i < 7; ++i) appendPod<double>(bytes, 0.0);
    appendPod<uint32_t>(bytes, vertexCount);

    for (uint32_t i = 0; i < vertexCount; ++i) {
        appendPod<uint16_t>(bytes, 0);
    }
    for (uint32_t i = 0; i < vertexCount; ++i) {
        appendPod<uint16_t>(bytes, 0);
    }
    for (uint32_t i = 0; i < vertexCount; ++i) {
        appendPod<uint16_t>(bytes, 0);
    }

    if ((bytes.size() % 4u) != 0u) {
        appendPod<uint16_t>(bytes, 0);
    }
    appendPod<uint32_t>(bytes, 1);
    appendPod<uint32_t>(bytes, 0);
    appendPod<uint32_t>(bytes, 0);
    appendPod<uint32_t>(bytes, 0);

    appendPod<uint32_t>(bytes, 2);
    appendPod<uint32_t>(bytes, 0);
    appendPod<uint32_t>(bytes, highEdgeIndex);
    for (int i = 0; i < 3; ++i) {
        appendPod<uint32_t>(bytes, 0);
    }
    return bytes;
}

std::vector<uint8_t> makeLargeQuantizedMeshBytesMissingUint32IndexPadding() {
    std::vector<uint8_t> bytes =
        makeLargeQuantizedMeshBytesWithUint32EdgeIndex();
    constexpr uint32_t vertexCount = 65537u;
    constexpr size_t paddingOffset =
        92 + static_cast<size_t>(vertexCount) * 3u * sizeof(uint16_t);
    bytes.erase(
        bytes.begin() + static_cast<std::ptrdiff_t>(paddingOffset),
        bytes.begin() + static_cast<std::ptrdiff_t>(
                            paddingOffset + sizeof(uint16_t)));
    return bytes;
}

std::vector<uint8_t> makeLargeUint16QuantizedMeshBytesWithSkirts() {
    constexpr uint32_t vertexCount = 65535u;
    std::vector<uint8_t> bytes;
    bytes.reserve(92u + vertexCount * 6u + 64u);

    for (int i = 0; i < 3; ++i) appendPod<double>(bytes, 0.0);
    appendPod<float>(bytes, 0.0f);
    appendPod<float>(bytes, 100.0f);
    for (int i = 0; i < 7; ++i) appendPod<double>(bytes, 0.0);
    appendPod<uint32_t>(bytes, vertexCount);

    appendPod<uint16_t>(bytes, zigZagEncode16(0));
    appendPod<uint16_t>(bytes, zigZagEncode16(32767));
    appendPod<uint16_t>(bytes, zigZagEncode16(-32767));
    for (uint32_t i = 3; i < vertexCount; ++i) {
        appendPod<uint16_t>(bytes, zigZagEncode16(0));
    }

    appendPod<uint16_t>(bytes, zigZagEncode16(0));
    appendPod<uint16_t>(bytes, zigZagEncode16(0));
    appendPod<uint16_t>(bytes, zigZagEncode16(32767));
    for (uint32_t i = 3; i < vertexCount; ++i) {
        appendPod<uint16_t>(bytes, zigZagEncode16(0));
    }

    for (uint32_t i = 0; i < vertexCount; ++i) {
        appendPod<uint16_t>(bytes, zigZagEncode16(0));
    }

    appendPod<uint32_t>(bytes, 1);
    appendPod<uint16_t>(bytes, 0);
    appendPod<uint16_t>(bytes, 0);
    appendPod<uint16_t>(bytes, 0);

    auto appendEdge = [&](uint16_t a, uint16_t b) {
        appendPod<uint32_t>(bytes, 2u);
        appendPod<uint16_t>(bytes, a);
        appendPod<uint16_t>(bytes, b);
    };
    appendEdge(0, 2);
    appendEdge(1, 0);
    appendEdge(1, 2);
    appendEdge(2, 1);
    return bytes;
}

std::vector<uint8_t> makeQuantizedMeshBytesWithHeaderPadding(
    const std::string& metadataJson) {
    std::vector<uint8_t> bytes = makeQuantizedMeshBytes(metadataJson);
    bytes.insert(bytes.begin() + 92, 4, 0);
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

    std::unique_ptr<SurfaceTileMesh> mesh =
        QuantizedMeshParser::parseToSurfaceTileMesh(
            bytes.data(),
            bytes.size(),
            rootRectangle());

    ASSERT_NE(nullptr, mesh);
    ASSERT_EQ(2u, mesh->metadataAvailability.size());
    EXPECT_EQ((QuantizedMeshAvailabilityRange{0, 0, 0, 1, 0}),
              mesh->metadataAvailability[0]);
    EXPECT_EQ((QuantizedMeshAvailabilityRange{1, 2, 1, 3, 1}),
              mesh->metadataAvailability[1]);

    EXPECT_EQ(mesh->metadataAvailability,
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

    std::unique_ptr<SurfaceTileMesh> mesh =
        QuantizedMeshParser::parseToSurfaceTileMesh(
            bytes.data(),
            bytes.size(),
            rootRectangle());

    ASSERT_NE(nullptr, mesh);
    EXPECT_EQ(3u, mesh->vertices.size());
    EXPECT_TRUE(mesh->indices.empty());
    EXPECT_EQ((std::vector<QuantizedMeshAvailabilityRange>{
                  {0, 0, 0, 1, 0}}),
              mesh->metadataAvailability);
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
    std::unique_ptr<SurfaceTileMesh> shortNormalMesh =
        QuantizedMeshParser::parseToSurfaceTileMesh(
            shortNormalBytes.data(),
            shortNormalBytes.size(),
            rootRectangle());
    ASSERT_NE(nullptr, shortNormalMesh);
    EXPECT_EQ((std::vector<QuantizedMeshAvailabilityRange>{
                  {0, 0, 0, 1, 0}}),
              shortNormalMesh->metadataAvailability);

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

TEST(QuantizedMeshParserValidationTest,
     RejectsTruncatedEdgeIndicesLikeCesiumNative) {
    std::vector<uint8_t> bytes = makeQuantizedMeshBytes("", true);
    std::unique_ptr<SurfaceTileMesh> validMesh =
        QuantizedMeshParser::parseToSurfaceTileMesh(
            bytes.data(),
            bytes.size(),
            rootRectangle());
    ASSERT_NE(nullptr, validMesh);

    bytes.pop_back();
    EXPECT_EQ(nullptr,
              QuantizedMeshParser::parseToSurfaceTileMesh(
                  bytes.data(),
                  bytes.size(),
                  rootRectangle()));
    EXPECT_EQ(nullptr,
              QuantizedMeshParser::parseAndRasterize(
                  bytes.data(),
                  bytes.size(),
                  64));
}

TEST(QuantizedMeshParserValidationTest,
     RasterizerAcceptsZeroTriangleMeshLikeCesiumNative) {
    const std::vector<uint8_t> bytes = makeZeroTriangleQuantizedMeshBytes();
    std::unique_ptr<DecodedHeightmap> heightmap =
        QuantizedMeshParser::parseAndRasterize(
            bytes.data(),
            bytes.size(),
            8);

    ASSERT_NE(nullptr, heightmap);
    EXPECT_EQ(9, heightmap->tileSize);
    EXPECT_EQ(81u, heightmap->heights.size());
    EXPECT_DOUBLE_EQ(0.0, heightmap->minHeight);
    EXPECT_DOUBLE_EQ(100.0, heightmap->maxHeight);
}

TEST(QuantizedMeshParserValidationTest,
     RejectsIllFormedCoreBuffersLikeCesiumNative) {
    std::vector<uint8_t> shortHeader(32);
    EXPECT_EQ(nullptr,
              QuantizedMeshParser::parseToSurfaceTileMesh(
                  shortHeader.data(),
                  shortHeader.size(),
                  rootRectangle()));

    const std::vector<uint8_t> validBytes = makeQuantizedMeshBytes();
    constexpr size_t afterUBuffer = 92 + 3 * sizeof(uint16_t);
    std::vector<uint8_t> truncatedVertexData(
        validBytes.begin(),
        validBytes.begin() + static_cast<std::ptrdiff_t>(afterUBuffer));
    EXPECT_EQ(nullptr,
              QuantizedMeshParser::parseToSurfaceTileMesh(
                  truncatedVertexData.data(),
                  truncatedVertexData.size(),
                  rootRectangle()));
    EXPECT_EQ(nullptr,
              QuantizedMeshParser::parseAndRasterize(
                  truncatedVertexData.data(),
                  truncatedVertexData.size(),
                  64));

    constexpr size_t afterTriangleCount =
        92 + 3 * 3 * sizeof(uint16_t) + sizeof(uint32_t);
    std::vector<uint8_t> truncatedIndices(
        validBytes.begin(),
        validBytes.begin() + static_cast<std::ptrdiff_t>(afterTriangleCount));
    EXPECT_EQ(nullptr,
              QuantizedMeshParser::parseToSurfaceTileMesh(
                  truncatedIndices.data(),
                  truncatedIndices.size(),
                  rootRectangle()));
    EXPECT_EQ(nullptr,
              QuantizedMeshParser::parseAndRasterize(
                  truncatedIndices.data(),
                  truncatedIndices.size(),
                  64));
}

TEST(QuantizedMeshParserValidationTest,
     RejectsZeroVertexCountLikeCesiumNative) {
    const std::vector<uint8_t> bytes = makeZeroVertexCountQuantizedMeshBytes();

    EXPECT_EQ(nullptr,
              QuantizedMeshParser::parseToSurfaceTileMesh(
                  bytes.data(),
                  bytes.size(),
                  rootRectangle()));
    EXPECT_EQ(nullptr,
              QuantizedMeshParser::parseAndRasterize(
                  bytes.data(),
                  bytes.size(),
                  64));
    EXPECT_TRUE(QuantizedMeshParser::parseMetadataAvailability(
                    bytes.data(),
                    bytes.size())
                    .empty());
}

TEST(QuantizedMeshParserValidationTest,
     RasterizerRejectsHeaderWithoutVertexCountLikeCesiumNative) {
    for (const size_t byteCount : {size_t{88}, size_t{91}}) {
        std::vector<uint8_t> headerWithoutVertexCount(byteCount);

        EXPECT_EQ(nullptr,
                  QuantizedMeshParser::parseAndRasterize(
                      headerWithoutVertexCount.data(),
                      headerWithoutVertexCount.size(),
                      64));
    }
}

TEST(QuantizedMeshParserValidationTest,
     RejectsEachTruncatedEdgeLikeCesiumNative) {
    const std::vector<uint8_t> validBytes = makeQuantizedMeshBytes("", true);
    constexpr size_t edgeStart =
        92 + 3 * 3 * sizeof(uint16_t) +
        sizeof(uint32_t) + 3 * sizeof(uint16_t);
    constexpr size_t edgeByteLength =
        sizeof(uint32_t) + 2 * sizeof(uint16_t);

    const std::array<std::pair<const char*, size_t>, 4> edgeCuts{{
        {"west", edgeStart + sizeof(uint32_t)},
        {"south", edgeStart + edgeByteLength + sizeof(uint32_t)},
        {"east", edgeStart + edgeByteLength * 2 + sizeof(uint32_t)},
        {"north", edgeStart + edgeByteLength * 3 + sizeof(uint32_t)}
    }};

    for (const auto& [edgeName, byteCount] : edgeCuts) {
        SCOPED_TRACE(edgeName);
        std::vector<uint8_t> truncatedBytes(
            validBytes.begin(),
            validBytes.begin() + static_cast<std::ptrdiff_t>(byteCount));

        EXPECT_EQ(nullptr,
                  QuantizedMeshParser::parseToSurfaceTileMesh(
                      truncatedBytes.data(),
                      truncatedBytes.size(),
                      rootRectangle()));
        EXPECT_EQ(nullptr,
                  QuantizedMeshParser::parseAndRasterize(
                      truncatedBytes.data(),
                      truncatedBytes.size(),
                      64));
    }
}

TEST(QuantizedMeshParserIndexWidthTest,
     VertexCountAbove65536ParsesUint32IndicesAndEdges) {
    const std::vector<uint8_t> bytes =
        makeLargeQuantizedMeshBytesWithUint32EdgeIndex();

    std::unique_ptr<SurfaceTileMesh> mesh =
        QuantizedMeshParser::parseToSurfaceTileMesh(
            bytes.data(),
            bytes.size(),
            rootRectangle());

    ASSERT_NE(nullptr, mesh);
    EXPECT_GT(mesh->vertices.size(), 65537u);
    EXPECT_NE(mesh->indices.end(),
              std::find(mesh->indices.begin(), mesh->indices.end(), 65536u));
}

TEST(QuantizedMeshParserIndexWidthTest,
     RasterizerAcceptsUint32IndexPaddingLikeCesiumNative) {
    const std::vector<uint8_t> bytes =
        makeLargeQuantizedMeshBytesWithUint32EdgeIndex();

    std::unique_ptr<DecodedHeightmap> heightmap =
        QuantizedMeshParser::parseAndRasterize(
            bytes.data(),
            bytes.size(),
            1);

    ASSERT_NE(nullptr, heightmap);
    EXPECT_EQ(2, heightmap->tileSize);
    EXPECT_EQ(4u, heightmap->heights.size());
}

TEST(QuantizedMeshParserIndexWidthTest,
     MetadataOnlyPathAcceptsUint32IndexPaddingLikeCesiumNative) {
    const std::string metadata = R"json({
      "available": [
        [{"startX":0,"startY":0,"endX":1,"endY":1}]
      ]
    })json";
    std::vector<uint8_t> bytes =
        makeLargeQuantizedMeshBytesWithUint32EdgeIndex();
    appendMetadataExtension(bytes, metadata);

    EXPECT_EQ((std::vector<QuantizedMeshAvailabilityRange>{
                  {0, 0, 0, 1, 1}}),
              QuantizedMeshParser::parseMetadataAvailability(
                  bytes.data(),
                  bytes.size()));
}

TEST(QuantizedMeshParserIndexWidthTest,
     VertexCount65536StillUsesUint16IndicesLikeCesiumNative) {
    const std::string metadata = R"json({
      "available": [
        [{"startX":0,"startY":0,"endX":1,"endY":0}]
      ]
    })json";
    const std::vector<uint8_t> bytes =
        makeBoundaryUint16QuantizedMeshBytes(metadata);

    std::unique_ptr<SurfaceTileMesh> mesh =
        QuantizedMeshParser::parseToSurfaceTileMesh(
            bytes.data(),
            bytes.size(),
            rootRectangle());

    ASSERT_NE(nullptr, mesh);
    EXPECT_NE(mesh->indices.end(),
              std::find(mesh->indices.begin(), mesh->indices.end(), 65535u));
    EXPECT_EQ((std::vector<QuantizedMeshAvailabilityRange>{
                  {0, 0, 0, 1, 0}}),
              QuantizedMeshParser::parseMetadataAvailability(
                  bytes.data(),
                  bytes.size()));
}

TEST(QuantizedMeshParserIndexWidthTest,
     RejectsMissingUint32IndexPaddingLikeCesiumNative) {
    const std::vector<uint8_t> bytes =
        makeLargeQuantizedMeshBytesMissingUint32IndexPadding();

    EXPECT_EQ(nullptr,
              QuantizedMeshParser::parseToSurfaceTileMesh(
                  bytes.data(),
                  bytes.size(),
                  rootRectangle()));
}

TEST(QuantizedMeshParserIndexWidthTest,
     RasterizerRejectsMissingUint32IndexPaddingLikeCesiumNative) {
    const std::vector<uint8_t> bytes =
        makeLargeQuantizedMeshBytesMissingUint32IndexPadding();

    EXPECT_EQ(nullptr,
              QuantizedMeshParser::parseAndRasterize(
                  bytes.data(),
                  bytes.size(),
                  64));
}

TEST(QuantizedMeshParserSkirtTest,
     SkirtNormalsCopyProvidedEdgeNormalsLikeCesiumNative) {
    const std::vector<uint8_t> bytes =
        makeQuantizedMeshBytes("", true, true);

    std::unique_ptr<SurfaceTileMesh> mesh =
        QuantizedMeshParser::parseToSurfaceTileMesh(
            bytes.data(),
            bytes.size(),
            rootRectangle());

    ASSERT_NE(nullptr, mesh);
    const uint32_t firstSkirtVertex =
        mesh->skirtMeta.noSkirtVerticesBegin +
        mesh->skirtMeta.noSkirtVerticesCount;
    ASSERT_LT(firstSkirtVertex, mesh->vertices.size());

    EXPECT_LT((mesh->vertices[firstSkirtVertex].normalEcef -
               mesh->vertices[0].normalEcef)
                  .length(),
              1e-12);
}

TEST(QuantizedMeshParserSkirtTest,
     SkirtNormalsCopyEachGeneratedEdgeSourceNormalLikeCesiumNative) {
    const std::vector<uint8_t> bytes =
        makeQuantizedMeshBytes("", true, false);

    std::unique_ptr<SurfaceTileMesh> mesh =
        QuantizedMeshParser::parseToSurfaceTileMesh(
            bytes.data(),
            bytes.size(),
            rootRectangle());

    ASSERT_NE(nullptr, mesh);
    ASSERT_GE(mesh->vertices.size(), 11u);
    EXPECT_LT((mesh->vertices[3].normalEcef -
               mesh->vertices[0].normalEcef)
                  .length(),
              1e-12);
    EXPECT_LT((mesh->vertices[5].normalEcef -
               mesh->vertices[1].normalEcef)
                  .length(),
              1e-12);
    EXPECT_LT((mesh->vertices[7].normalEcef -
               mesh->vertices[2].normalEcef)
                  .length(),
              1e-12);
    EXPECT_LT((mesh->vertices[9].normalEcef -
               mesh->vertices[2].normalEcef)
                  .length(),
              1e-12);
}

TEST(QuantizedMeshParserSkirtTest,
     GeneratedFallbackNormalsPointOutwardLikeCesiumNative) {
    const std::vector<uint8_t> bytes = makeQuantizedMeshBytes();

    std::unique_ptr<SurfaceTileMesh> mesh =
        QuantizedMeshParser::parseToSurfaceTileMesh(
            bytes.data(),
            bytes.size(),
            rootRectangle());

    ASSERT_NE(nullptr, mesh);
    ASSERT_GT(mesh->skirtMeta.noSkirtVerticesCount, 0u);
    const auto& ellipsoid = Ellipsoid::WGS84();
    for (size_t i = 0; i < mesh->skirtMeta.noSkirtVerticesCount; ++i) {
        const Vec3 geodeticNormal =
            ellipsoid.geodeticSurfaceNormal(mesh->vertices[i].positionEcef);
        EXPECT_GE(mesh->vertices[i].normalEcef.dot(geodeticNormal), 0.0);
    }
}

TEST(QuantizedMeshParserSkirtTest,
     SkirtCountsMatchCesiumNativeFormula) {
    const std::vector<uint8_t> bytes =
        makeQuantizedMeshBytes("", true, false);

    std::unique_ptr<SurfaceTileMesh> mesh =
        QuantizedMeshParser::parseToSurfaceTileMesh(
            bytes.data(),
            bytes.size(),
            rootRectangle());

    constexpr uint32_t coreVertexCount = 3;
    constexpr uint32_t coreIndexCount = 3;
    constexpr uint32_t totalSkirtVertices = 8;
    constexpr uint32_t totalSkirtIndices = (totalSkirtVertices - 4) * 6;
    ASSERT_NE(nullptr, mesh);
    EXPECT_EQ(coreVertexCount, mesh->skirtMeta.noSkirtVerticesCount);
    EXPECT_EQ(coreIndexCount, mesh->skirtMeta.noSkirtIndicesCount);
    EXPECT_EQ(static_cast<size_t>(coreVertexCount + totalSkirtVertices),
              mesh->vertices.size());
    EXPECT_EQ(static_cast<size_t>(coreIndexCount + totalSkirtIndices),
              mesh->indices.size());
}

TEST(QuantizedMeshParserSkirtTest,
     SkirtIndicesFollowCesiumNativeSortedEdgeTriangleOrder) {
    const std::vector<uint8_t> bytes =
        makeQuantizedMeshBytes("", true, false);

    std::unique_ptr<SurfaceTileMesh> mesh =
        QuantizedMeshParser::parseToSurfaceTileMesh(
            bytes.data(),
            bytes.size(),
            rootRectangle());

    const std::vector<uint32_t> expectedSkirtIndices = {
        0, 2, 3, 3, 2, 4,
        1, 0, 5, 5, 0, 6,
        2, 1, 7, 7, 1, 8,
        2, 1, 9, 9, 1, 10
    };

    ASSERT_NE(nullptr, mesh);
    ASSERT_GE(mesh->indices.size(),
              mesh->skirtMeta.noSkirtIndicesCount +
                  expectedSkirtIndices.size());
    const std::vector<uint32_t> actualSkirtIndices(
        mesh->indices.begin() + mesh->skirtMeta.noSkirtIndicesCount,
        mesh->indices.begin() + mesh->skirtMeta.noSkirtIndicesCount +
            static_cast<std::ptrdiff_t>(expectedSkirtIndices.size()));

    EXPECT_EQ(expectedSkirtIndices, actualSkirtIndices);
}

TEST(QuantizedMeshParserSkirtTest,
     SkirtVerticesExpandOutsideTileEdgesLikeCesiumNative) {
    auto scheme = TileScheme::createGeographicTMS();
    const TileKey interiorKey{"Geographic-TMS", 2, 1, 1};
    const Rectangle bounds = scheme->tileToRectangle(interiorKey);
    const std::vector<uint8_t> bytes =
        makeQuantizedMeshBytes("", true, false);

    std::unique_ptr<SurfaceTileMesh> mesh =
        QuantizedMeshParser::parseToSurfaceTileMesh(
            bytes.data(),
            bytes.size(),
            bounds);

    ASSERT_NE(nullptr, mesh);
    ASSERT_GE(mesh->vertices.size(), 11u);

    const auto& ellipsoid = Ellipsoid::WGS84();
    const Cartographic west =
        ellipsoid.cartesianToCartographic(mesh->vertices[3].positionEcef);
    const Cartographic westTop =
        ellipsoid.cartesianToCartographic(mesh->vertices[0].positionEcef);
    const Cartographic south =
        ellipsoid.cartesianToCartographic(mesh->vertices[5].positionEcef);
    const Cartographic southTop =
        ellipsoid.cartesianToCartographic(mesh->vertices[1].positionEcef);
    const Cartographic east =
        ellipsoid.cartesianToCartographic(mesh->vertices[7].positionEcef);
    const Cartographic eastTop =
        ellipsoid.cartesianToCartographic(mesh->vertices[2].positionEcef);
    const Cartographic north =
        ellipsoid.cartesianToCartographic(mesh->vertices[9].positionEcef);
    const Cartographic northTop =
        ellipsoid.cartesianToCartographic(mesh->vertices[2].positionEcef);
    const double longitudeOffset = (bounds.west() - bounds.east()) * 0.0001;
    const double latitudeOffset = (bounds.north() - bounds.south()) * 0.0001;

    EXPECT_LT(
        std::abs((west.longitude() - westTop.longitude()) - longitudeOffset),
        1e-8);
    EXPECT_LT(std::abs(west.latitude() - westTop.latitude()), 1e-8);
    EXPECT_LT(
        std::abs((south.latitude() - southTop.latitude()) + latitudeOffset),
        1e-8);
    EXPECT_LT(std::abs(south.longitude() - southTop.longitude()), 1e-8);
    EXPECT_LT(
        std::abs((east.longitude() - eastTop.longitude()) - longitudeOffset),
        1e-8);
    EXPECT_LT(std::abs(east.latitude() - eastTop.latitude()), 1e-8);
    EXPECT_LT(
        std::abs((north.latitude() - northTop.latitude()) - latitudeOffset),
        1e-8);
    EXPECT_LT(std::abs(north.longitude() - northTop.longitude()), 1e-8);
}

TEST(QuantizedMeshParserSkirtTest,
     SkirtHeightMatchesCesiumNativeGeometricErrorFormula) {
    const Rectangle bounds = rootRectangle();
    const std::vector<uint8_t> bytes =
        makeQuantizedMeshBytes("", true, true);

    std::unique_ptr<SurfaceTileMesh> mesh =
        QuantizedMeshParser::parseToSurfaceTileMesh(
            bytes.data(),
            bytes.size(),
            bounds);

    ASSERT_NE(nullptr, mesh);
    const uint32_t firstSkirtVertex =
        mesh->skirtMeta.noSkirtVerticesBegin +
        mesh->skirtMeta.noSkirtVerticesCount;
    ASSERT_LT(firstSkirtVertex, mesh->vertices.size());

    const auto& ellipsoid = Ellipsoid::WGS84();
    const Cartographic top =
        ellipsoid.cartesianToCartographic(mesh->vertices[0].positionEcef);
    const Cartographic skirt =
        ellipsoid.cartesianToCartographic(
            mesh->vertices[firstSkirtVertex].positionEcef);
    const double expectedSkirtHeight =
        ellipsoid.semiMajorAxis() * 0.25 / 65.0 * bounds.width() * 5.0;
    EXPECT_LT(std::abs((top.height() - skirt.height()) - expectedSkirtHeight),
              1e-6);
}

TEST(QuantizedMeshParserSkirtTest,
     Uint16SourceIndicesPromoteWhenSkirtsExceedUint16VertexRange) {
    const std::vector<uint8_t> bytes =
        makeLargeUint16QuantizedMeshBytesWithSkirts();

    std::unique_ptr<SurfaceTileMesh> mesh =
        QuantizedMeshParser::parseToSurfaceTileMesh(
            bytes.data(),
            bytes.size(),
            rootRectangle());

    ASSERT_NE(nullptr, mesh);
    const auto maxIndexIt =
        std::max_element(mesh->indices.begin(), mesh->indices.end());
    ASSERT_NE(mesh->indices.end(), maxIndexIt);
    EXPECT_EQ(65535u, mesh->skirtMeta.noSkirtVerticesCount);
    EXPECT_GT(mesh->vertices.size(), 65535u);
    EXPECT_GT(*maxIndexIt,
              static_cast<uint32_t>(std::numeric_limits<uint16_t>::max()));
}

TEST(QuantizedMeshParserOctNormalTest,
     DecodesAxisAlignedOctEncodedNormalsLikeCesiumNative) {
    const std::vector<uint8_t> bytes =
        makeQuantizedMeshBytes("", true, true);

    std::unique_ptr<SurfaceTileMesh> mesh =
        QuantizedMeshParser::parseToSurfaceTileMesh(
            bytes.data(),
            bytes.size(),
            rootRectangle());

    ASSERT_NE(nullptr, mesh);
    ASSERT_GT(mesh->vertices.size(), 3u);
    EXPECT_GT(mesh->vertices[0].normalEcef.z(), 0.9999);
    EXPECT_LT(std::abs(mesh->vertices[0].normalEcef.x()), 0.004);
    EXPECT_LT(std::abs(mesh->vertices[0].normalEcef.y()), 0.004);
    EXPECT_GT(mesh->vertices[1].normalEcef.x(), 0.9999);
    EXPECT_LT(std::abs(mesh->vertices[1].normalEcef.y()), 0.004);
    EXPECT_LT(std::abs(mesh->vertices[1].normalEcef.z()), 0.004);
    EXPECT_GT(mesh->vertices[2].normalEcef.y(), 0.9999);
    EXPECT_LT(std::abs(mesh->vertices[2].normalEcef.x()), 0.004);
    EXPECT_LT(std::abs(mesh->vertices[2].normalEcef.z()), 0.004);

    const uint32_t firstSkirtVertex =
        mesh->skirtMeta.noSkirtVerticesBegin +
        mesh->skirtMeta.noSkirtVerticesCount;
    ASSERT_LT(firstSkirtVertex, mesh->vertices.size());
    EXPECT_LT((mesh->vertices[firstSkirtVertex].normalEcef -
               mesh->vertices[0].normalEcef)
                  .length(),
              1e-12);
}

TEST(QuantizedMeshParserOctNormalTest,
     PreservesArbitraryOctEncodedDirectionLikeCesiumNative) {
    std::vector<uint8_t> bytes =
        makeQuantizedMeshBytes("", true, false);

    appendPod<uint8_t>(bytes, 1);
    appendPod<uint32_t>(bytes, 6);
    const uint8_t encodedNormal[] = {
        141, 221,
        141, 221,
        141, 221
    };
    bytes.insert(bytes.end(),
                 encodedNormal,
                 encodedNormal + sizeof(encodedNormal));

    std::unique_ptr<SurfaceTileMesh> mesh =
        QuantizedMeshParser::parseToSurfaceTileMesh(
            bytes.data(),
            bytes.size(),
            rootRectangle());

    const Vec3 expected(0.13834289277321496,
                        0.9684002494125046,
                        0.20751433915982243);
    ASSERT_NE(nullptr, mesh);
    ASSERT_FALSE(mesh->vertices.empty());
    for (const SurfaceVertex& vertex : mesh->vertices) {
        EXPECT_LT((vertex.normalEcef - expected).length(), 0.006);
    }
}

TEST(QuantizedMeshParserOctNormalTest,
     IgnoresTrailingOctNormalExtensionBytesLikeCesiumNative) {
    std::vector<uint8_t> bytes =
        makeQuantizedMeshBytes("", true, false);

    appendPod<uint8_t>(bytes, 1);
    appendPod<uint32_t>(bytes, 8);
    const uint8_t normals[] = {
        128, 255,
        128, 255,
        128, 255,
        0, 0
    };
    bytes.insert(bytes.end(), normals, normals + sizeof(normals));

    std::unique_ptr<SurfaceTileMesh> mesh =
        QuantizedMeshParser::parseToSurfaceTileMesh(
            bytes.data(),
            bytes.size(),
            rootRectangle());

    ASSERT_NE(nullptr, mesh);
    ASSERT_GE(mesh->vertices.size(), 3u);
    EXPECT_GT(mesh->vertices[0].normalEcef.y(), 0.9999);
    EXPECT_LT(std::abs(mesh->vertices[0].normalEcef.x()), 0.004);
    EXPECT_LT(std::abs(mesh->vertices[0].normalEcef.z()), 0.004);
}

TEST(QuantizedMeshParserOctNormalTest,
     LaterOctNormalExtensionReplacesEarlierLikeCesiumNative) {
    std::vector<uint8_t> bytes =
        makeQuantizedMeshBytes("", true, false);

    appendPod<uint8_t>(bytes, 1);
    appendPod<uint32_t>(bytes, 6);
    const uint8_t firstNormals[] = {
        128, 255,
        128, 255,
        128, 255
    };
    bytes.insert(bytes.end(),
                 firstNormals,
                 firstNormals + sizeof(firstNormals));

    appendPod<uint8_t>(bytes, 1);
    appendPod<uint32_t>(bytes, 6);
    const uint8_t secondNormals[] = {
        255, 128,
        255, 128,
        255, 128
    };
    bytes.insert(bytes.end(),
                 secondNormals,
                 secondNormals + sizeof(secondNormals));

    std::unique_ptr<SurfaceTileMesh> mesh =
        QuantizedMeshParser::parseToSurfaceTileMesh(
            bytes.data(),
            bytes.size(),
            rootRectangle());

    ASSERT_NE(nullptr, mesh);
    ASSERT_FALSE(mesh->vertices.empty());
    EXPECT_GT(mesh->vertices[0].normalEcef.x(), 0.9999);
    EXPECT_LT(std::abs(mesh->vertices[0].normalEcef.y()), 0.004);
    EXPECT_LT(std::abs(mesh->vertices[0].normalEcef.z()), 0.004);
}

TEST(QuantizedMeshParserValidationTest,
     RejectsDecodedTriangleIndicesOutsideVertexRange) {
    std::vector<uint8_t> bytes =
        makeQuantizedMeshBytes("", false, true);
    constexpr size_t thirdTriangleIndexOffset =
        92 + 3 * 3 * sizeof(uint16_t) + sizeof(uint32_t) +
        2 * sizeof(uint16_t);
    const uint16_t invalidHighWaterMarkCode = 0xffffu;
    std::memcpy(
        bytes.data() + thirdTriangleIndexOffset,
        &invalidHighWaterMarkCode,
        sizeof(invalidHighWaterMarkCode));

    EXPECT_EQ(nullptr,
              QuantizedMeshParser::parseToSurfaceTileMesh(
                  bytes.data(),
                  bytes.size(),
                  rootRectangle()));
    EXPECT_EQ(nullptr,
              QuantizedMeshParser::parseAndRasterize(
                  bytes.data(),
                  bytes.size(),
                  64));
}

TEST(QuantizedMeshParserValidationTest,
     RejectsEdgeIndicesOutsideVertexRange) {
    std::vector<uint8_t> bytes =
        makeQuantizedMeshBytes("", true, true);
    constexpr size_t firstWestEdgeIndexOffset =
        92 + 3 * 3 * sizeof(uint16_t) + sizeof(uint32_t) +
        3 * sizeof(uint16_t) + sizeof(uint32_t);
    const uint16_t invalidEdgeIndex = 99u;
    std::memcpy(
        bytes.data() + firstWestEdgeIndexOffset,
        &invalidEdgeIndex,
        sizeof(invalidEdgeIndex));

    EXPECT_EQ(nullptr,
              QuantizedMeshParser::parseToSurfaceTileMesh(
                  bytes.data(),
                  bytes.size(),
                  rootRectangle()));
    EXPECT_EQ(nullptr,
              QuantizedMeshParser::parseAndRasterize(
                  bytes.data(),
                  bytes.size(),
                  64));
}
