#include <gtest/gtest.h>

#include "earth_engine/content/QuantizedMeshContentLoader.h"
#include "earth_engine/core/geodesy/Cartographic.h"
#include "earth_engine/core/geodesy/Ellipsoid.h"
#include "earth_engine/core/math/MathUtils.h"
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

QuantizedMeshContentLoadResult loadQuantizedMeshContent(
    const std::vector<uint8_t>& bytes,
    const Rectangle& rectangle = rootRectangle(),
    RasterOverlayProjection terrainProjection =
        RasterOverlayProjection::Geographic) {
    return QuantizedMeshContentLoader::load(
        bytes.data(),
        bytes.size(),
        rectangle,
        false,
        {},
        terrainProjection);
}

} // namespace

TEST(QuantizedMeshContentLoaderHeaderTest,
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

    QuantizedMeshContentLoadResult result =
        loadQuantizedMeshContent(bytes, bounds);

    ASSERT_TRUE(result.success());
    ASSERT_TRUE(result.gltfModel->preferredLocalOriginEcef.has_value());
    EXPECT_LT((*result.gltfModel->preferredLocalOriginEcef -
               boundingSphereCenter)
                  .length(),
              1e-12);
    ASSERT_EQ(1u, result.gltfModel->nodes.size());
    EXPECT_EQ(Mat4::translation(boundingSphereCenter),
              result.gltfModel->nodes.front().globalTransform);

    const Vec3 expectedFirstVertex =
        Ellipsoid::WGS84().cartographicToCartesian(
            Cartographic::fromRadians(bounds.west(), bounds.south(), 0.0));
    ASSERT_EQ(1u, result.gltfModel->primitives.size());
    const GltfPrimitive& primitive = result.gltfModel->primitives.front();
    ASSERT_FALSE(primitive.vertices.empty());
    EXPECT_LT((primitive.vertices[0].positionEcef - expectedFirstVertex)
                  .length(),
              1e-6);
    ASSERT_EQ(primitive.vertices.size(), primitive.runtime.baseVertices.size());
    EXPECT_LT((primitive.runtime.baseVertices[0].positionEcef -
               (expectedFirstVertex - boundingSphereCenter))
                  .length(),
              1e-6);
}

TEST(QuantizedMeshContentLoaderHeaderTest,
     HeaderHeightRangeIsExposedLikeCesiumNative) {
    constexpr float minimumHeight = -123.5f;
    constexpr float maximumHeight = 456.25f;
    const std::vector<uint8_t> bytes =
        makeQuantizedMeshBytes(
            Vec3::zero(),
            minimumHeight,
            maximumHeight);

    QuantizedMeshContentLoadResult result =
        loadQuantizedMeshContent(bytes);

    ASSERT_TRUE(result.success());
    ASSERT_TRUE(result.metadata.terrainHeightRange.has_value());
    EXPECT_LT(std::abs(result.metadata.terrainHeightRange->first -
                       minimumHeight),
              1e-6);
    EXPECT_LT(std::abs(result.metadata.terrainHeightRange->second -
                       maximumHeight),
              1e-6);
    ASSERT_TRUE(result.metadata.updatedBoundingVolume.has_value());
    EXPECT_LT(std::abs(result.metadata.updatedBoundingVolume->minimumHeight -
                       minimumHeight),
              1e-6);
    EXPECT_LT(std::abs(result.metadata.updatedBoundingVolume->maximumHeight -
                       maximumHeight),
              1e-6);
}

TEST(QuantizedMeshContentLoaderHeaderTest,
     NonzeroHorizonOcclusionPointIsPreservedLikeCesiumNative) {
    const Vec3 horizonOcclusionPoint(0.25, -0.5, 0.75);
    const std::vector<uint8_t> bytes =
        makeQuantizedMeshBytes(
            Vec3::zero(),
            0.0f,
            100.0f,
            Vec3::zero(),
            horizonOcclusionPoint);

    QuantizedMeshContentLoadResult result =
        loadQuantizedMeshContent(bytes);

    ASSERT_TRUE(result.success());
    ASSERT_TRUE(result.metadata.horizonOcclusionPoint.has_value());
    EXPECT_LT((*result.metadata.horizonOcclusionPoint - horizonOcclusionPoint)
                  .length(),
              1e-12);
}

TEST(QuantizedMeshContentLoaderHeaderTest,
     ZeroHorizonOcclusionPointIsStillExposedLikeCesiumNative) {
    const std::vector<uint8_t> bytes =
        makeQuantizedMeshBytes(
            Vec3::zero(),
            0.0f,
            100.0f,
            Vec3::zero(),
            Vec3::zero());

    QuantizedMeshContentLoadResult result =
        loadQuantizedMeshContent(bytes);

    ASSERT_TRUE(result.success());
    ASSERT_TRUE(result.metadata.horizonOcclusionPoint.has_value());
    EXPECT_EQ(Vec3::zero(), *result.metadata.horizonOcclusionPoint);
}

TEST(QuantizedMeshContentLoaderVertexDecodeTest,
     UvAndHeightGoldenMatchCesiumNative) {
    const Rectangle bounds = rootRectangle();
    constexpr float minimumHeight = -50.0f;
    constexpr float maximumHeight = 150.0f;
    const std::vector<uint8_t> bytes =
        makeQuantizedMeshBytes(
            Vec3::zero(),
            minimumHeight,
            maximumHeight);

    QuantizedMeshContentLoadResult result =
        loadQuantizedMeshContent(bytes, bounds);

    ASSERT_TRUE(result.success());
    ASSERT_EQ(1u, result.gltfModel->primitives.size());
    const GltfPrimitive& primitive = result.gltfModel->primitives.front();
    ASSERT_EQ(3u, primitive.vertices.size());
    EXPECT_EQ(3u, primitive.indices.size());

    EXPECT_NEAR(0.0f, primitive.vertices[0].uv[0], 1e-6f);
    EXPECT_NEAR(1.0f, primitive.vertices[0].uv[1], 1e-6f);
    EXPECT_NEAR(1.0f, primitive.vertices[1].uv[0], 1e-6f);
    EXPECT_NEAR(1.0f, primitive.vertices[1].uv[1], 1e-6f);
    EXPECT_NEAR(0.0f, primitive.vertices[2].uv[0], 1e-6f);
    EXPECT_NEAR(0.0f, primitive.vertices[2].uv[1], 1e-6f);

    const auto& ellipsoid = Ellipsoid::WGS84();
    const Cartographic sw =
        ellipsoid.cartesianToCartographic(primitive.vertices[0].positionEcef);
    const Cartographic se =
        ellipsoid.cartesianToCartographic(primitive.vertices[1].positionEcef);
    const Cartographic nw =
        ellipsoid.cartesianToCartographic(primitive.vertices[2].positionEcef);

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

TEST(QuantizedMeshContentLoaderVertexDecodeTest,
     WebMercatorOverlayUvKeepsEastAntimeridianLikeCesiumNative) {
    const Rectangle bounds(
        MathUtils::OnePi - 1e-8,
        -0.1,
        MathUtils::OnePi,
        0.1);
    const std::vector<uint8_t> bytes = makeQuantizedMeshBytes();

    QuantizedMeshContentLoadResult result =
        loadQuantizedMeshContent(
            bytes,
            bounds,
            RasterOverlayProjection::WebMercator);

    ASSERT_TRUE(result.success());
    ASSERT_EQ(1u, result.gltfModel->primitives.size());
    const GltfPrimitive& primitive = result.gltfModel->primitives.front();
    ASSERT_EQ(primitive.vertices.size(), primitive.vertexTexCoords[0].size());
    ASSERT_GE(primitive.vertexTexCoords[0].size(), 2u);
    EXPECT_NEAR(1.0f, primitive.vertexTexCoords[0][1][0], 1e-6f);
    EXPECT_NEAR(0.0f, primitive.vertexTexCoords[0][1][1], 1e-6f);
}

TEST(QuantizedMeshContentLoaderVertexDecodeTest,
     WebMercatorOverlayUvKeepsWestAntimeridianLikeCesiumNative) {
    const Rectangle bounds(
        -MathUtils::OnePi,
        -0.1,
        -MathUtils::OnePi + 1e-8,
        0.1);
    const std::vector<uint8_t> bytes = makeQuantizedMeshBytes();

    QuantizedMeshContentLoadResult result =
        loadQuantizedMeshContent(
            bytes,
            bounds,
            RasterOverlayProjection::WebMercator);

    ASSERT_TRUE(result.success());
    ASSERT_EQ(1u, result.gltfModel->primitives.size());
    const GltfPrimitive& primitive = result.gltfModel->primitives.front();
    ASSERT_EQ(primitive.vertices.size(), primitive.vertexTexCoords[0].size());
    ASSERT_GE(primitive.vertexTexCoords[0].size(), 3u);
    EXPECT_NEAR(0.0f, primitive.vertexTexCoords[0][0][0], 1e-6f);
    EXPECT_NEAR(0.0f, primitive.vertexTexCoords[0][2][0], 1e-6f);
}
