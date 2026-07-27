#include <gtest/gtest.h>

#include "earth_engine/providers/TerrainProvider.h"
#include "earth_engine/tiling/LoadedTerrainHeightSampler.h"
#include "earth_engine/tiling/TileScheme.h"
#include "earth_engine/tiling/TilesetTile.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

using namespace earth_engine;

namespace {

std::string cacheKeyFor(const TileKey& key) {
    return key.schemeId.str() + ":" +
           std::to_string(key.z) + ":" +
           std::to_string(key.x) + ":" +
           std::to_string(key.y);
}

// A 2x2 heightmap with a single uniform height. These tests exercise the
// tile-SELECTION semantics of LoadedTerrainHeightSampler (coverage → nullopt,
// deepest-tile-wins, ancestor fallback, equal-detail highest-wins); the
// per-sample bilinear/bounds/no-data math is covered separately in
// test_decoded_heightmap_sampler.cpp, so uniform maps keep these assertions
// free of interpolation ambiguity.
std::unique_ptr<DecodedHeightmap> makeUniformHeightmap(float height) {
    auto heightmap = std::make_unique<DecodedHeightmap>();
    heightmap->tileSize = 2;
    heightmap->heights = {height, height, height, height};
    heightmap->minHeight = height;
    heightmap->maxHeight = height;
    return heightmap;
}

// Install a loaded terrain tile whose height comes from a retained
// DecodedHeightmap (the GPU-displacement source of truth), mirroring how real
// heightmap terrain tiles are delivered.
void putLoadedHeightmapTerrainTile(
    std::unordered_map<std::string, std::unique_ptr<TilesetTile>>& tiles,
    const TileKey& key,
    const Rectangle& bounds,
    std::unique_ptr<DecodedHeightmap> heightmap) {
    auto tile = std::make_unique<TilesetTile>(key, bounds);
    tile->content.renderContent.setTerrainRenderContent(true);
    tile->content.renderContent.setRetainedHeightmap(std::move(heightmap));
    tiles.emplace(cacheKeyFor(key), std::move(tile));
}

} // namespace

TEST(LoadedTerrainHeightSamplerTest, ReturnsNulloptWhenNoLoadedTerrainCovers) {
    auto scheme = TileScheme::createGeographicTMS();
    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;

    // A loaded terrain tile exists, but it covers a DIFFERENT quadrant than the
    // query point → no covering sample → nullopt (must NOT be reported as 0.0,
    // which is a real sea-level height).
    const TileKey coveredKey{"Geographic-TMS", 1, 0, 0};
    const Rectangle coveredBounds = scheme->tileToRectangle(coveredKey);
    putLoadedHeightmapTerrainTile(
        tiles, coveredKey, coveredBounds, makeUniformHeightmap(55.0f));

    const TileKey elsewhereKey{"Geographic-TMS", 1, 1, 1};
    const Rectangle elsewhere = scheme->tileToRectangle(elsewhereKey);
    const double longitude = (elsewhere.west() + elsewhere.east()) * 0.5;
    const double latitude = (elsewhere.south() + elsewhere.north()) * 0.5;

    const std::optional<float> sample =
        LoadedTerrainHeightSampler::sampleHeightOptional(
            tiles, longitude, latitude);
    EXPECT_FALSE(sample.has_value());

    // The convenience wrapper still falls back to sea level for such callers.
    EXPECT_NEAR(
        0.0f,
        LoadedTerrainHeightSampler::sampleHeight(tiles, longitude, latitude),
        1e-4f);
}

TEST(LoadedTerrainHeightSamplerTest, UsesBestLoadedTerrainTile) {
    auto scheme = TileScheme::createGeographicTMS();
    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    const TileKey childKey{"Geographic-TMS", 1, 0, 0};
    const Rectangle rootBounds = scheme->tileToRectangle(rootKey);
    const Rectangle childBounds = scheme->tileToRectangle(childKey);
    putLoadedHeightmapTerrainTile(
        tiles, rootKey, rootBounds, makeUniformHeightmap(10.0f));
    putLoadedHeightmapTerrainTile(
        tiles, childKey, childBounds, makeUniformHeightmap(42.0f));

    const double longitude = (childBounds.west() + childBounds.east()) * 0.5;
    const double latitude = (childBounds.south() + childBounds.north()) * 0.5;

    // Both root and child cover the point; the deeper (child) tile wins.
    EXPECT_NEAR(
        42.0f,
        LoadedTerrainHeightSampler::sampleHeight(tiles, longitude, latitude),
        1e-4f);
}

TEST(LoadedTerrainHeightSamplerTest, FallsBackToLoadedAncestorTerrain) {
    auto scheme = TileScheme::createGeographicTMS();
    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    const TileKey childKey{"Geographic-TMS", 1, 0, 0};
    const Rectangle rootBounds = scheme->tileToRectangle(rootKey);
    const Rectangle childBounds = scheme->tileToRectangle(childKey);
    // Only root has a retained heightmap; the deeper child has no terrain data
    // (e.g. an upsampled child with no own heightmap) → scan must fall back to
    // the covering ancestor rather than returning nullopt.
    putLoadedHeightmapTerrainTile(
        tiles, rootKey, rootBounds, makeUniformHeightmap(123.0f));
    tiles.emplace(cacheKeyFor(childKey),
                  std::make_unique<TilesetTile>(childKey, childBounds));

    const double longitude = (childBounds.west() + childBounds.east()) * 0.5;
    const double latitude = (childBounds.south() + childBounds.north()) * 0.5;

    EXPECT_NEAR(
        123.0f,
        LoadedTerrainHeightSampler::sampleHeight(tiles, longitude, latitude),
        1e-4f);
}

TEST(LoadedTerrainHeightSamplerTest, SamplesUniformHeightmapTerrainTile) {
    auto scheme = TileScheme::createGeographicTMS();
    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    const Rectangle bounds = scheme->tileToRectangle(rootKey);
    putLoadedHeightmapTerrainTile(
        tiles, rootKey, bounds, makeUniformHeightmap(37.5f));

    const double longitude = bounds.west() + bounds.width() * 0.25;
    const double latitude = bounds.south() + bounds.height() * 0.25;

    EXPECT_NEAR(
        37.5f,
        LoadedTerrainHeightSampler::sampleHeight(tiles, longitude, latitude),
        1e-4f);
}

TEST(LoadedTerrainHeightSamplerTest,
     SamplesHighestTerrainSurfaceAcrossEqualDetailTilesLikeCesiumNative) {
    auto scheme = TileScheme::createGeographicTMS();
    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;

    // Two equal-detail tiles cover the same point (a transition/overlap seam);
    // the higher surface wins, matching Cesium-native height sampling.
    const TileKey lowerKey{"Geographic-TMS", 1, 0, 0};
    const TileKey upperKey{"Geographic-TMS", 1, 1, 0};
    const Rectangle bounds = scheme->tileToRectangle(lowerKey);
    putLoadedHeightmapTerrainTile(
        tiles, lowerKey, bounds, makeUniformHeightmap(78.0f));
    putLoadedHeightmapTerrainTile(
        tiles, upperKey, bounds, makeUniformHeightmap(83.0f));

    const double longitude = bounds.west() + bounds.width() * 0.25;
    const double latitude = bounds.south() + bounds.height() * 0.25;

    EXPECT_NEAR(
        83.0f,
        LoadedTerrainHeightSampler::sampleHeight(tiles, longitude, latitude),
        1e-4f);
}

// === TerrainAncestorHeightSource:fill 代理的高度来源 ===
// 代理顶点全部落在自己的矩形内,而同级邻居与该矩形相邻、不相交 —— 真正覆盖
// 代理全域的只有祖先链。以下三条钉住"沿 parent 链找最近可用祖先"的语义。

TEST(TerrainAncestorHeightSourceTest, FindsNearestAncestorWithHeightmap) {
    auto scheme = TileScheme::createGeographicTMS();

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    const TileKey midKey{"Geographic-TMS", 1, 0, 0};
    const TileKey leafKey{"Geographic-TMS", 2, 0, 0};

    TilesetTile root(rootKey, scheme->tileToRectangle(rootKey));
    root.content.renderContent.setTerrainRenderContent(true);
    root.content.renderContent.setRetainedHeightmap(
        makeUniformHeightmap(10.0f));

    TilesetTile mid(midKey, scheme->tileToRectangle(midKey));
    mid.content.renderContent.setTerrainRenderContent(true);
    mid.content.renderContent.setRetainedHeightmap(
        makeUniformHeightmap(600.0f));
    mid.parent = &root;

    // leaf 自身还在加载(无 heightmap)—— 正是需要 fill 代理的状态。
    TilesetTile leaf(leafKey, scheme->tileToRectangle(leafKey));
    leaf.parent = &mid;

    const TilesetTile* source = TerrainAncestorHeightSource::find(leaf);
    ASSERT_NE(nullptr, source);
    EXPECT_EQ(midKey, source->key);

    const Rectangle leafBounds = scheme->tileToRectangle(leafKey);
    const double longitude = (leafBounds.west() + leafBounds.east()) * 0.5;
    const double latitude = (leafBounds.south() + leafBounds.north()) * 0.5;
    const std::optional<float> height =
        TerrainAncestorHeightSource::sample(*source, longitude, latitude);
    ASSERT_TRUE(height.has_value());
    EXPECT_NEAR(600.0f, *height, 1e-4f);
}

TEST(TerrainAncestorHeightSourceTest, SkipsAncestorsWithoutHeightmap) {
    auto scheme = TileScheme::createGeographicTMS();

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    const TileKey midKey{"Geographic-TMS", 1, 0, 0};
    const TileKey leafKey{"Geographic-TMS", 2, 0, 0};

    TilesetTile root(rootKey, scheme->tileToRectangle(rootKey));
    root.content.renderContent.setTerrainRenderContent(true);
    root.content.renderContent.setRetainedHeightmap(
        makeUniformHeightmap(10.0f));

    // 中间层自己也是 fill / 上采样(无 retained heightmap)→ 必须继续上溯,
    // 否则代理会退回海平面。
    TilesetTile mid(midKey, scheme->tileToRectangle(midKey));
    mid.parent = &root;

    TilesetTile leaf(leafKey, scheme->tileToRectangle(leafKey));
    leaf.parent = &mid;

    const TilesetTile* source = TerrainAncestorHeightSource::find(leaf);
    ASSERT_NE(nullptr, source);
    EXPECT_EQ(rootKey, source->key);
}

TEST(TerrainAncestorHeightSourceTest, ReturnsNullWhenNoAncestorLoaded) {
    auto scheme = TileScheme::createGeographicTMS();
    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    TilesetTile root(rootKey, scheme->tileToRectangle(rootKey));

    // 无祖先可借 → nullptr,调用方保持平椭球(不能假装 0 是真高度)。
    EXPECT_EQ(nullptr, TerrainAncestorHeightSource::find(root));
}
