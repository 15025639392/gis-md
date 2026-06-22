#include <gtest/gtest.h>

#include "earth_engine/content/QuantizedMeshContentLoader.h"
#include "earth_engine/terrain/QuantizedMeshParser.h"
#include "earth_engine/tiling/TileKey.h"
#include "earth_engine/tiling/TileScheme.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
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

std::vector<uint8_t> makeQuantizedMeshBytes(
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

std::vector<uint8_t> makeZeroTriangleQuantizedMeshBytes() {
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

Rectangle rootRectangle() {
    auto scheme = TileScheme::createGeographicTMS();
    return scheme->tileToRectangle(TileKey{"Geographic-TMS", 0, 0, 0});
}

QuantizedMeshContentLoadResult loadQuantizedMeshContent(
    const std::vector<uint8_t>& bytes) {
    return QuantizedMeshContentLoader::load(
        bytes.data(),
        bytes.size(),
        rootRectangle(),
        false,
        {});
}

} // namespace

TEST(QuantizedMeshContentLoaderValidationTest,
     RejectsTruncatedEdgeIndicesLikeCesiumNative) {
    std::vector<uint8_t> bytes = makeQuantizedMeshBytes(true);
    QuantizedMeshContentLoadResult validResult =
        loadQuantizedMeshContent(bytes);
    ASSERT_TRUE(validResult.success());

    bytes.pop_back();
    EXPECT_FALSE(loadQuantizedMeshContent(bytes).success());
}

TEST(QuantizedMeshContentLoaderValidationTest,
     GltfContentAcceptsZeroTriangleMeshLikeCesiumNative) {
    const std::vector<uint8_t> bytes = makeZeroTriangleQuantizedMeshBytes();
    QuantizedMeshContentLoadResult result = loadQuantizedMeshContent(bytes);

    ASSERT_TRUE(result.success());
    ASSERT_EQ(1u, result.gltfModel->primitives.size());
    const GltfPrimitive& primitive = result.gltfModel->primitives.front();
    EXPECT_TRUE(primitive.indices.empty());
    ASSERT_TRUE(result.metadata.terrainHeightRange.has_value());
    EXPECT_DOUBLE_EQ(0.0, result.metadata.terrainHeightRange->first);
    EXPECT_DOUBLE_EQ(100.0, result.metadata.terrainHeightRange->second);
}

TEST(QuantizedMeshContentLoaderValidationTest,
     RejectsIllFormedCoreBuffersLikeCesiumNative) {
    std::vector<uint8_t> shortHeader(32);
    EXPECT_FALSE(loadQuantizedMeshContent(shortHeader).success());

    const std::vector<uint8_t> validBytes = makeQuantizedMeshBytes();
    constexpr size_t afterUBuffer = 92 + 3 * sizeof(uint16_t);
    std::vector<uint8_t> truncatedVertexData(
        validBytes.begin(),
        validBytes.begin() + static_cast<std::ptrdiff_t>(afterUBuffer));
    EXPECT_FALSE(loadQuantizedMeshContent(truncatedVertexData).success());

    constexpr size_t afterTriangleCount =
        92 + 3 * 3 * sizeof(uint16_t) + sizeof(uint32_t);
    std::vector<uint8_t> truncatedIndices(
        validBytes.begin(),
        validBytes.begin() + static_cast<std::ptrdiff_t>(afterTriangleCount));
    EXPECT_FALSE(loadQuantizedMeshContent(truncatedIndices).success());
}

TEST(QuantizedMeshContentLoaderValidationTest,
     RejectsZeroVertexCountLikeCesiumNative) {
    const std::vector<uint8_t> bytes = makeZeroVertexCountQuantizedMeshBytes();

    EXPECT_FALSE(loadQuantizedMeshContent(bytes).success());
    EXPECT_TRUE(QuantizedMeshParser::parseMetadataAvailability(
                    bytes.data(),
                    bytes.size())
                    .empty());
}

TEST(QuantizedMeshContentLoaderValidationTest,
     RejectsHeaderWithoutVertexCountLikeCesiumNative) {
    for (const size_t byteCount : {size_t{88}, size_t{91}}) {
        std::vector<uint8_t> headerWithoutVertexCount(byteCount);

        EXPECT_FALSE(loadQuantizedMeshContent(headerWithoutVertexCount)
                         .success());
    }
}

TEST(QuantizedMeshContentLoaderValidationTest,
     RejectsEachTruncatedEdgeLikeCesiumNative) {
    const std::vector<uint8_t> validBytes = makeQuantizedMeshBytes(true);
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

        EXPECT_FALSE(loadQuantizedMeshContent(truncatedBytes).success());
    }
}

TEST(QuantizedMeshContentLoaderValidationTest,
     RejectsDecodedTriangleIndicesOutsideVertexRange) {
    std::vector<uint8_t> bytes = makeQuantizedMeshBytes(false, true);
    constexpr size_t thirdTriangleIndexOffset =
        92 + 3 * 3 * sizeof(uint16_t) + sizeof(uint32_t) +
        2 * sizeof(uint16_t);
    const uint16_t invalidHighWaterMarkCode = 0xffffu;
    std::memcpy(
        bytes.data() + thirdTriangleIndexOffset,
        &invalidHighWaterMarkCode,
        sizeof(invalidHighWaterMarkCode));

    EXPECT_FALSE(loadQuantizedMeshContent(bytes).success());
}

TEST(QuantizedMeshContentLoaderValidationTest,
     RejectsEdgeIndicesOutsideVertexRange) {
    std::vector<uint8_t> bytes = makeQuantizedMeshBytes(true, true);
    constexpr size_t firstWestEdgeIndexOffset =
        92 + 3 * 3 * sizeof(uint16_t) + sizeof(uint32_t) +
        3 * sizeof(uint16_t) + sizeof(uint32_t);
    const uint16_t invalidEdgeIndex = 99u;
    std::memcpy(
        bytes.data() + firstWestEdgeIndexOffset,
        &invalidEdgeIndex,
        sizeof(invalidEdgeIndex));

    EXPECT_FALSE(loadQuantizedMeshContent(bytes).success());
}
