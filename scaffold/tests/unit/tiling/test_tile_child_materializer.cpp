#include <gtest/gtest.h>

#include "earth_engine/tiling/RasterMappedToTilesetTile.h"
#include "earth_engine/tiling/TileChildMaterializer.h"

#include <memory>
#include <string>
#include <unordered_map>

using namespace earth_engine;

namespace {

std::string cacheKeyFor(const TileKey& key) {
    return key.schemeId + ":" +
           std::to_string(key.z) + ":" +
           std::to_string(key.x) + ":" +
           std::to_string(key.y);
}

} // namespace

TEST(TileChildMaterializerTest, LinkContentChildrenWithoutDuplicates) {
    const TileKey parentKey{"test", 0, 0, 0};
    const TileKey firstKey{"test", 1, 0, 0};
    const TileKey secondKey{"test", 1, 1, 0};
    TilesetTile parent(parentKey, Rectangle{});

    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;
    tiles.emplace(
        "test:1:0:0",
        std::make_unique<TilesetTile>(firstKey, Rectangle{}));
    tiles.emplace(
        "test:1:1:0",
        std::make_unique<TilesetTile>(secondKey, Rectangle{}));

    auto ensure = [&tiles](const TileKey& key) -> TilesetTile* {
        auto it = tiles.find(cacheKeyFor(key));
        return it == tiles.end() ? nullptr : it->second.get();
    };
    const std::vector<TileKey> childKeys{firstKey, secondKey, firstKey};

    const bool changed =
        TileChildMaterializer::linkContentChildren(parent, childKeys, ensure);
    const bool changedAgain =
        TileChildMaterializer::linkContentChildren(parent, childKeys, ensure);

    EXPECT_TRUE(changed);
    EXPECT_FALSE(changedAgain);
    ASSERT_EQ(2u, parent.children.size());
    EXPECT_EQ(tiles["test:1:0:0"].get(), parent.children[0]);
    EXPECT_EQ(tiles["test:1:1:0"].get(), parent.children[1]);
    EXPECT_EQ(&parent, parent.children[0]->parent);
    EXPECT_EQ(&parent, parent.children[1]->parent);
}

TEST(TileChildMaterializerTest, AnyAvailableTerrainChildCreatesFullQuadLikeCesiumNative) {
    TilesetTile parent(
        TileKey{"Geographic-TMS", 0, 0, 0},
        Rectangle{});
    parent.geometricError = 100.0;
    parent.refine = TileRefine::Add;
    parent.content.renderContent.setTerrainHeightRange(-10.0, 90.0);

    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;
    auto ensure = [&tiles](const TileKey& key) -> TilesetTile* {
        const std::string cacheKey = cacheKeyFor(key);
        auto it = tiles.find(cacheKey);
        if (it == tiles.end()) {
            it = tiles.emplace(
                cacheKey,
                std::make_unique<TilesetTile>(key, Rectangle{})).first;
        }
        return it->second.get();
    };
    auto availability = [](const TileKey& key) {
        return key.x == 0 && key.y == 0
            ? TileAvailabilityState::Available
            : TileAvailabilityState::NotAvailable;
    };

    const bool changed = TileChildMaterializer::materializeTerrainChildren(
        parent,
        2,
        availability,
        ensure);

    ASSERT_TRUE(changed);
    ASSERT_EQ(4u, parent.children.size());

    TilesetTile* sw = parent.children[0];
    TilesetTile* se = parent.children[1];
    TilesetTile* nw = parent.children[2];
    TilesetTile* ne = parent.children[3];

    EXPECT_FALSE(sw->content.upsampledFromParent);
    EXPECT_TRUE(se->content.upsampledFromParent);
    EXPECT_TRUE(nw->content.upsampledFromParent);
    EXPECT_TRUE(ne->content.upsampledFromParent);

    EXPECT_DOUBLE_EQ(50.0, sw->geometricError);
    EXPECT_DOUBLE_EQ(50.0, se->geometricError);
    EXPECT_EQ(TileRefine::Add, sw->refine);
    EXPECT_EQ(TileRefine::Add, se->refine);
    EXPECT_TRUE(sw->content.renderContent.hasTerrainHeightRange());
    EXPECT_TRUE(se->content.renderContent.hasTerrainHeightRange());
    EXPECT_DOUBLE_EQ(-10.0, sw->content.renderContent.terrainMinimumHeight());
    EXPECT_DOUBLE_EQ(90.0, se->content.renderContent.terrainMaximumHeight());
}

TEST(TileChildMaterializerTest, RasterUpsampledChildrenSplitSubdivisionAndRemainStable) {
    TilesetTile parent(
        TileKey{"Geographic-TMS", 0, 0, 0},
        Rectangle::fromDegrees(-20.0, -10.0, 0.0, 10.0));
    parent.geometricError = 100.0;
    parent.content.renderContent.setTerrainHeightRange(-5.0, 25.0);

    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;
    auto ensure = [&tiles](const TileKey& key) -> TilesetTile* {
        const std::string cacheKey = cacheKeyFor(key);
        auto it = tiles.find(cacheKey);
        if (it == tiles.end()) {
            it = tiles.emplace(
                cacheKey,
                std::make_unique<TilesetTile>(key, Rectangle{})).first;
        }
        return it->second.get();
    };

    const Rectangle subdivision =
        Rectangle::fromDegrees(-20.0, -10.0, 0.0, 10.0);
    const bool changed =
        TileChildMaterializer::materializeRasterUpsampledChildren(
            parent,
            subdivision,
            200.0,
            ensure);
    const bool changedAgain =
        TileChildMaterializer::materializeRasterUpsampledChildren(
            parent,
            subdivision,
            200.0,
            ensure);

    ASSERT_TRUE(changed);
    EXPECT_FALSE(changedAgain);
    ASSERT_EQ(4u, parent.children.size());
    EXPECT_EQ(TileRefine::Replace, parent.refine);

    EXPECT_EQ(
        Rectangle::fromDegrees(-20.0, -10.0, -10.0, 0.0),
        parent.children[0]->bounds);
    EXPECT_EQ(
        Rectangle::fromDegrees(-10.0, 0.0, 0.0, 10.0),
        parent.children[3]->bounds);

    for (TilesetTile* child : parent.children) {
        ASSERT_NE(nullptr, child);
        EXPECT_EQ(&parent, child->parent);
        EXPECT_TRUE(child->content.upsampledFromParent);
        EXPECT_TRUE(child->content.rasterUpsampledForMoreDetail);
        EXPECT_DOUBLE_EQ(50.0, child->geometricError);
        ASSERT_TRUE(child->boundingVolume.has_value());
        EXPECT_EQ(TileBoundingVolumeKind::Region, child->boundingVolume->kind);
        EXPECT_TRUE(child->content.renderContent.hasTerrainHeightRange());
        EXPECT_DOUBLE_EQ(
            -5.0,
            child->content.renderContent.terrainMinimumHeight());
        EXPECT_DOUBLE_EQ(
            25.0,
            child->content.renderContent.terrainMaximumHeight());
    }
}

TEST(TileChildMaterializerTest, RasterUpsampledTileCanContinueSubdividingForImageryDetail) {
    TilesetTile parent(
        TileKey{"Geographic-TMS", 10, 512, 512},
        Rectangle::fromDegrees(106.0, 29.0, 107.0, 30.0));
    parent.geometricError = 64.0;
    parent.content.upsampledFromParent = true;
    parent.content.rasterUpsampledForMoreDetail = true;
    parent.content.renderContent.setTerrainHeightRange(100.0, 500.0);

    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;
    auto ensure = [&tiles](const TileKey& key) -> TilesetTile* {
        const std::string cacheKey = cacheKeyFor(key);
        auto it = tiles.find(cacheKey);
        if (it == tiles.end()) {
            it = tiles.emplace(
                cacheKey,
                std::make_unique<TilesetTile>(key, Rectangle{})).first;
        }
        return it->second.get();
    };

    const bool changed =
        TileChildMaterializer::materializeRasterUpsampledChildren(
            parent,
            parent.bounds,
            64.0,
            ensure);

    ASSERT_TRUE(changed);
    ASSERT_EQ(4u, parent.children.size());
    for (TilesetTile* child : parent.children) {
        ASSERT_NE(nullptr, child);
        EXPECT_EQ(&parent, child->parent);
        EXPECT_TRUE(child->content.upsampledFromParent);
        EXPECT_TRUE(child->content.rasterUpsampledForMoreDetail);
        EXPECT_DOUBLE_EQ(32.0, child->geometricError);
        EXPECT_TRUE(child->content.renderContent.hasTerrainHeightRange());
        EXPECT_DOUBLE_EQ(
            100.0,
            child->content.renderContent.terrainMinimumHeight());
        EXPECT_DOUBLE_EQ(
            500.0,
            child->content.renderContent.terrainMaximumHeight());
    }

    EXPECT_TRUE(TileChildMaterializer::canRefine(
        parent,
        TileRefinementAvailabilityOptions{
            true,
            false,
            false,
            false,
            true,
            18},
        [](const TileKey&) { return std::string{"child"}; },
        [](const std::string&) { return false; },
        [](const TileKey&) { return TileAvailabilityState::NotAvailable; }));
}

TEST(TileChildMaterializerTest, CanRefineHonorsContentRulesBeforeTerrainSignals) {
    TilesetTile tile(TileKey{"test", 0, 0, 0}, Rectangle{});
    auto noCacheKey = [](const TileKey&) { return std::string{}; };
    auto noTerrainCached = [](const std::string&) { return false; };
    auto noAvailability = [](const TileKey&) {
        return TileAvailabilityState::NotAvailable;
    };

    EXPECT_TRUE(TileChildMaterializer::canRefine(
        tile,
        TileRefinementAvailabilityOptions{
            false,
            true,
            false,
            false,
            false,
            4},
        noCacheKey,
        noTerrainCached,
        noAvailability));

    EXPECT_FALSE(TileChildMaterializer::canRefine(
        tile,
        TileRefinementAvailabilityOptions{
            false,
            false,
            true,
            false,
            true,
            4},
        [](const TileKey&) { return std::string{"child"}; },
        [](const std::string&) { return true; },
        [](const TileKey&) { return TileAvailabilityState::Available; }));
}

TEST(TileChildMaterializerTest, CanRefineUsesCachedAndAvailableTerrainSignals) {
    TilesetTile tile(TileKey{"Geographic-TMS", 0, 0, 0}, Rectangle{});

    EXPECT_TRUE(TileChildMaterializer::canRefine(
        tile,
        TileRefinementAvailabilityOptions{
            false,
            false,
            false,
            false,
            false,
            4},
        cacheKeyFor,
        [](const std::string& cacheKey) {
            return cacheKey == "Geographic-TMS:1:0:0";
        },
        [](const TileKey&) { return TileAvailabilityState::NotAvailable; }));

    EXPECT_TRUE(TileChildMaterializer::canRefine(
        tile,
        TileRefinementAvailabilityOptions{
            false,
            false,
            false,
            false,
            true,
            4},
        cacheKeyFor,
        [](const std::string&) { return false; },
        [](const TileKey& key) {
            return key.x == 1 && key.y == 0
                ? TileAvailabilityState::Available
                : TileAvailabilityState::NotAvailable;
        }));
}

TEST(TileChildMaterializerTest, CanRefineBlocksAvailabilityBoundaryAndTerrainUpsampledTiles) {
    TilesetTile tile(TileKey{"Geographic-TMS", 0, 0, 0}, Rectangle{});

    EXPECT_FALSE(TileChildMaterializer::canRefine(
        tile,
        TileRefinementAvailabilityOptions{
            false,
            false,
            false,
            true,
            true,
            4},
        [](const TileKey&) { return std::string{"child"}; },
        [](const std::string&) { return true; },
        [](const TileKey&) { return TileAvailabilityState::Available; }));

    tile.content.upsampledFromParent = true;
    tile.content.rasterUpsampledForMoreDetail = false;
    EXPECT_FALSE(TileChildMaterializer::canRefine(
        tile,
        TileRefinementAvailabilityOptions{
            true,
            true,
            false,
            false,
            true,
            4},
        [](const TileKey&) { return std::string{"child"}; },
        [](const std::string&) { return true; },
        [](const TileKey&) { return TileAvailabilityState::Available; }));
}
