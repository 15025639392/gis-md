#include <gtest/gtest.h>

#include "earth_engine/core/geodesy/Cartographic.h"
#include "earth_engine/core/geodesy/Ellipsoid.h"
#include "earth_engine/terrain/QuantizedMeshParser.h"
#include "earth_engine/tiling/TileKey.h"
#include "earth_engine/tiling/TileScheme.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
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
    const Vec3& boundingSphereCenterEcef = Vec3::zero(),
    float minimumHeight = 0.0f,
    float maximumHeight = 100.0f,
    const Vec3& tileCenterEcef = Vec3::zero(),
    const Vec3& horizonOcclusionPoint = Vec3::zero(),
    const std::array<uint16_t, 3>& quantizedHeights = {0, 0, 0}) {
    std::vector<uint8_t> bytes;

    appendPod<double>(bytes, tileCenterEcef.x());
    appendPod<double>(bytes, tileCenterEcef.y());
    appendPod<double>(bytes, tileCenterEcef.z());
    appendPod<float>(bytes, minimumHeight);
    appendPod<float>(bytes, maximumHeight);
    appendPod<double>(bytes, boundingSphereCenterEcef.x());
    appendPod<double>(bytes, boundingSphereCenterEcef.y());
    appendPod<double>(bytes, boundingSphereCenterEcef.z());
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
        zigZagEncode16(static_cast<int32_t>(quantizedHeights[0])),
        zigZagEncode16(
            static_cast<int32_t>(quantizedHeights[1]) -
            static_cast<int32_t>(quantizedHeights[0])),
        zigZagEncode16(
            static_cast<int32_t>(quantizedHeights[2]) -
            static_cast<int32_t>(quantizedHeights[1]))
    };
    for (uint16_t value : u) appendPod<uint16_t>(bytes, value);
    for (uint16_t value : v) appendPod<uint16_t>(bytes, value);
    for (uint16_t value : h) appendPod<uint16_t>(bytes, value);

    appendPod<uint32_t>(bytes, 1);
    for (int i = 0; i < 3; ++i) appendPod<uint16_t>(bytes, 0);
    for (int i = 0; i < 4; ++i) appendPod<uint32_t>(bytes, 0);
    return bytes;
}

Rectangle rootRectangle() {
    auto scheme = TileScheme::createGeographicTMS();
    return scheme->tileToRectangle(TileKey{"Geographic-TMS", 0, 0, 0});
}

} // namespace

TEST(QuantizedMeshParserHeaderTest,
     RtcOriginComesFromBoundingSphereCenterLikeCesiumNative) {
    const Rectangle bounds = rootRectangle();
    const Vec3 boundingSphereCenter(1234.0, -5678.0, 9012.0);
    const Vec3 tileCenter(9999.0, 8888.0, 7777.0);
    const std::vector<uint8_t> bytes =
        makeQuantizedMeshBytes(
            boundingSphereCenter,
            0.0f,
            100.0f,
            tileCenter);

    std::unique_ptr<SurfaceTileMesh> mesh =
        QuantizedMeshParser::parseToSurfaceTileMesh(
            bytes.data(),
            bytes.size(),
            bounds);

    ASSERT_NE(nullptr, mesh);
    EXPECT_TRUE(mesh->hasLocalOriginEcef);
    EXPECT_LT((mesh->localOriginEcef - boundingSphereCenter).length(), 1e-12);

    const Vec3 expectedFirstVertex =
        Ellipsoid::WGS84().cartographicToCartesian(
            Cartographic::fromRadians(bounds.west(), bounds.south(), 0.0));
    ASSERT_FALSE(mesh->vertices.empty());
    EXPECT_LT((mesh->vertices[0].positionEcef - expectedFirstVertex).length(),
              1e-6);
}

TEST(QuantizedMeshParserHeaderTest,
     HeaderHeightRangeIsExposedLikeCesiumNative) {
    constexpr float minimumHeight = -123.5f;
    constexpr float maximumHeight = 456.25f;
    const std::vector<uint8_t> bytes =
        makeQuantizedMeshBytes(
            Vec3::zero(),
            minimumHeight,
            maximumHeight);

    std::unique_ptr<SurfaceTileMesh> mesh =
        QuantizedMeshParser::parseToSurfaceTileMesh(
            bytes.data(),
            bytes.size(),
            rootRectangle());

    ASSERT_NE(nullptr, mesh);
    EXPECT_TRUE(mesh->hasHeightRange);
    EXPECT_LT(std::abs(mesh->minimumHeight - minimumHeight), 1e-6);
    EXPECT_LT(std::abs(mesh->maximumHeight - maximumHeight), 1e-6);
}

TEST(QuantizedMeshParserHeaderTest,
     NonzeroHorizonOcclusionPointIsPreservedLikeCesiumNative) {
    const Vec3 horizonOcclusionPoint(0.25, -0.5, 0.75);
    const std::vector<uint8_t> bytes =
        makeQuantizedMeshBytes(
            Vec3::zero(),
            0.0f,
            100.0f,
            Vec3::zero(),
            horizonOcclusionPoint);

    std::unique_ptr<SurfaceTileMesh> mesh =
        QuantizedMeshParser::parseToSurfaceTileMesh(
            bytes.data(),
            bytes.size(),
            rootRectangle());

    ASSERT_NE(nullptr, mesh);
    EXPECT_TRUE(mesh->hasHorizonOcclusionPoint);
    EXPECT_LT((mesh->horizonOcclusionPoint - horizonOcclusionPoint).length(),
              1e-12);
}

TEST(QuantizedMeshParserHeaderTest,
     ZeroHorizonOcclusionPointIsStillExposedLikeCesiumNative) {
    const std::vector<uint8_t> bytes =
        makeQuantizedMeshBytes(
            Vec3::zero(),
            0.0f,
            100.0f,
            Vec3::zero(),
            Vec3::zero());

    std::unique_ptr<SurfaceTileMesh> mesh =
        QuantizedMeshParser::parseToSurfaceTileMesh(
            bytes.data(),
            bytes.size(),
            rootRectangle());

    ASSERT_NE(nullptr, mesh);
    EXPECT_TRUE(mesh->hasHorizonOcclusionPoint);
    EXPECT_EQ(Vec3::zero(), mesh->horizonOcclusionPoint);
}

TEST(QuantizedMeshParserVertexDecodeTest,
     UvAndHeightGoldenMatchCesiumNative) {
    const Rectangle bounds = rootRectangle();
    constexpr float minimumHeight = -50.0f;
    constexpr float maximumHeight = 150.0f;
    const std::vector<uint8_t> bytes =
        makeQuantizedMeshBytes(
            Vec3::zero(),
            minimumHeight,
            maximumHeight);

    std::unique_ptr<SurfaceTileMesh> mesh =
        QuantizedMeshParser::parseToSurfaceTileMesh(
            bytes.data(),
            bytes.size(),
            bounds);

    ASSERT_NE(nullptr, mesh);
    ASSERT_EQ(3u, mesh->vertices.size());
    EXPECT_EQ(3u, mesh->indices.size());

    EXPECT_NEAR(0.0f, mesh->vertices[0].uv[0], 1e-6f);
    EXPECT_NEAR(1.0f, mesh->vertices[0].uv[1], 1e-6f);
    EXPECT_NEAR(1.0f, mesh->vertices[1].uv[0], 1e-6f);
    EXPECT_NEAR(1.0f, mesh->vertices[1].uv[1], 1e-6f);
    EXPECT_NEAR(0.0f, mesh->vertices[2].uv[0], 1e-6f);
    EXPECT_NEAR(0.0f, mesh->vertices[2].uv[1], 1e-6f);

    const auto& ellipsoid = Ellipsoid::WGS84();
    const Cartographic sw =
        ellipsoid.cartesianToCartographic(mesh->vertices[0].positionEcef);
    const Cartographic se =
        ellipsoid.cartesianToCartographic(mesh->vertices[1].positionEcef);
    const Cartographic nw =
        ellipsoid.cartesianToCartographic(mesh->vertices[2].positionEcef);

    EXPECT_NEAR(bounds.west(), sw.longitude(), 1e-8);
    EXPECT_NEAR(bounds.south(), sw.latitude(), 1e-8);
    EXPECT_NEAR(minimumHeight, sw.height(), 1e-3);
    EXPECT_NEAR(bounds.east(), se.longitude(), 1e-8);
    EXPECT_NEAR(bounds.south(), se.latitude(), 1e-8);
    EXPECT_NEAR(minimumHeight, se.height(), 1e-3);
    EXPECT_NEAR(bounds.west(), nw.longitude(), 1e-8);
    EXPECT_NEAR(bounds.north(), nw.latitude(), 1e-8);
    EXPECT_NEAR(minimumHeight, nw.height(), 1e-3);
}

TEST(QuantizedMeshParserRasterizeTest,
     HeaderHeightRangeDrivesRasterizedHeightsLikeCesiumNative) {
    constexpr float minimumHeight = -80.0f;
    constexpr float maximumHeight = 120.0f;
    const std::vector<uint8_t> bytes =
        makeQuantizedMeshBytes(
            Vec3::zero(),
            minimumHeight,
            maximumHeight,
            Vec3::zero(),
            Vec3::zero(),
            {0, 32767, 16384});

    std::unique_ptr<DecodedHeightmap> heightmap =
        QuantizedMeshParser::parseAndRasterize(bytes.data(), bytes.size(), 1);

    ASSERT_NE(nullptr, heightmap);
    EXPECT_EQ(2, heightmap->tileSize);
    EXPECT_NEAR(minimumHeight, heightmap->minHeight, 1e-6);
    EXPECT_NEAR(maximumHeight, heightmap->maxHeight, 1e-6);
    ASSERT_EQ(4u, heightmap->heights.size());

    EXPECT_NEAR(minimumHeight, heightmap->heights[0], 1e-4f);
    EXPECT_NEAR(20.003f, heightmap->heights[1], 1e-3f);
    EXPECT_NEAR(maximumHeight, heightmap->heights[2], 1e-4f);
}
