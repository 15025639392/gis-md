#include <gtest/gtest.h>

#include "earth_engine/providers/QuantizedMeshTerrainProvider.h"
#include "earth_engine/tiling/TileCacheKey.h"
#include "earth_engine/tiling/TileScheme.h"
#include "earth_engine/tiling/Tileset.h"

#include <cstdint>
#include <memory>
#include <vector>

using namespace earth_engine;

namespace earth_engine {
struct TilesetTestAccess {
    static TilesetTile* ensureTile(Tileset& tileset, const TileKey& key) {
        return tileset.contentAccess_.ensureTile(key);
    }

    static void ensureTileMesh(Tileset& tileset, TilesetTile& tile) {
        tileset.meshPreparation_.ensureTileMesh(tile);
    }

    static void putTerrainCache(
        Tileset& tileset,
        const TileKey& key,
        std::unique_ptr<DecodedHeightmap> heightmap) {
        tileset.contentLifecycle_.terrainCache()[TileCacheKey::forTile(key)] =
            std::move(heightmap);
    }
};
} // namespace earth_engine

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
    const Vec3& boundingSphereCenterEcef,
    const Vec3& tileCenterEcef,
    float minimumHeight = 0.0f,
    float maximumHeight = 100.0f) {
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
    appendPod<double>(bytes, 0.0);
    appendPod<double>(bytes, 0.0);
    appendPod<double>(bytes, 0.0);
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
    appendPod<uint16_t>(bytes, 0);
    appendPod<uint16_t>(bytes, 0);
    appendPod<uint16_t>(bytes, 0);
    for (int i = 0; i < 4; ++i) appendPod<uint32_t>(bytes, 0);

    return bytes;
}

std::unique_ptr<DecodedHeightmap> makeFlatHeightmap(float heightMeters) {
    auto heightmap = std::make_unique<DecodedHeightmap>();
    heightmap->tileSize = 2;
    heightmap->heights.assign(4, heightMeters);
    heightmap->minHeight = heightMeters;
    heightmap->maxHeight = heightMeters;
    return heightmap;
}

TEST(TilesetQuantizedMeshTest,
     RtcOriginComesFromBoundingSphereCenterLikeCesiumNative) {
    auto provider = std::make_unique<QuantizedMeshTerrainProvider>(
        "https://example.invalid/fallback/{z}/{x}/{y}.terrain");
    auto scheme = TileScheme::createGeographicTMS();
    Tileset tileset(
        std::move(provider),
        std::move(scheme),
        {},
        nullptr,
        TilesetOptions{});

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    ASSERT_NE(nullptr, root);

    const Vec3 boundingSphereCenter(3456.0, -7890.0, 12345.0);
    const Vec3 tileCenter(-3456.0, 7890.0, -12345.0);
    auto heightmap = makeFlatHeightmap(0.0f);
    heightmap->rawData =
        makeQuantizedMeshBytes(boundingSphereCenter, tileCenter);
    TilesetTestAccess::putTerrainCache(
        tileset,
        rootKey,
        std::move(heightmap));

    TilesetTestAccess::ensureTileMesh(tileset, *root);

    EXPECT_NEAR(
        boundingSphereCenter.x(),
        root->content.renderContent.renderLocalOrigin().x(),
        1e-12);
    EXPECT_NEAR(
        boundingSphereCenter.y(),
        root->content.renderContent.renderLocalOrigin().y(),
        1e-12);
    EXPECT_NEAR(
        boundingSphereCenter.z(),
        root->content.renderContent.renderLocalOrigin().z(),
        1e-12);
}

TEST(TilesetQuantizedMeshTest,
     HeaderHeightRangeOverridesHeightmapFallbackLikeCesiumNative) {
    auto provider = std::make_unique<QuantizedMeshTerrainProvider>(
        "https://example.invalid/{z}/{x}/{y}.terrain");
    auto scheme = TileScheme::createGeographicTMS();
    Tileset tileset(std::move(provider), std::move(scheme), {}, nullptr, TilesetOptions{});

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    ASSERT_NE(nullptr, root);

    constexpr float minimumHeight = -250.0f;
    constexpr float maximumHeight = 1789.0f;
    auto heightmap = makeFlatHeightmap(0.0f);
    heightmap->rawData = makeQuantizedMeshBytes(
        Vec3::zero(),
        Vec3::zero(),
        minimumHeight,
        maximumHeight);
    TilesetTestAccess::putTerrainCache(
        tileset,
        rootKey,
        std::move(heightmap));

    TilesetTestAccess::ensureTileMesh(tileset, *root);

    EXPECT_TRUE(root->content.renderContent.hasTerrainHeightRange());
    EXPECT_NEAR(
        minimumHeight,
        root->content.renderContent.terrainMinimumHeight(),
        1e-6);
    EXPECT_NEAR(
        maximumHeight,
        root->content.renderContent.terrainMaximumHeight(),
        1e-6);
}

} // namespace
