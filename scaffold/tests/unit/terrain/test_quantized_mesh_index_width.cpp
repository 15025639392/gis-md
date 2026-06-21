#include <gtest/gtest.h>

#include "earth_engine/terrain/QuantizedMeshParser.h"
#include "earth_engine/tiling/TileKey.h"
#include "earth_engine/tiling/TileScheme.h"

#include <algorithm>
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

Rectangle rootRectangle() {
    auto scheme = TileScheme::createGeographicTMS();
    return scheme->tileToRectangle(TileKey{"Geographic-TMS", 0, 0, 0});
}

} // namespace

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
