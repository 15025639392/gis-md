#include <gtest/gtest.h>

#include "earth_engine/core/geodesy/Cartographic.h"
#include "earth_engine/core/geodesy/Ellipsoid.h"
#include "earth_engine/terrain/QuantizedMeshParser.h"
#include "earth_engine/tiling/TileKey.h"
#include "earth_engine/tiling/TileScheme.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
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

Rectangle rootRectangle() {
    auto scheme = TileScheme::createGeographicTMS();
    return scheme->tileToRectangle(TileKey{"Geographic-TMS", 0, 0, 0});
}

} // namespace

TEST(QuantizedMeshParserSkirtTest,
     SkirtNormalsCopyProvidedEdgeNormalsLikeCesiumNative) {
    const std::vector<uint8_t> bytes = makeQuantizedMeshBytes(true, true);

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
    const std::vector<uint8_t> bytes = makeQuantizedMeshBytes(true, false);

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
    const std::vector<uint8_t> bytes = makeQuantizedMeshBytes(true, false);

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
    const std::vector<uint8_t> bytes = makeQuantizedMeshBytes(true, false);

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
    const std::vector<uint8_t> bytes = makeQuantizedMeshBytes(true, false);

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
    const std::vector<uint8_t> bytes = makeQuantizedMeshBytes(true, true);

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
    const std::vector<uint8_t> bytes = makeQuantizedMeshBytes(true, true);

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
    std::vector<uint8_t> bytes = makeQuantizedMeshBytes(true, false);

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
    std::vector<uint8_t> bytes = makeQuantizedMeshBytes(true, false);

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
    std::vector<uint8_t> bytes = makeQuantizedMeshBytes(true, false);

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
