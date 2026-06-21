#include <gtest/gtest.h>

#include "earth_engine/tiling/TileContentAccess.h"
#include "earth_engine/tiling/TileContentLifecycleManager.h"
#include "earth_engine/tiling/TileLoadState.h"
#include "earth_engine/tiling/TileSelectionRootPolicy.h"
#include "earth_engine/tiling/TileScheme.h"
#include "earth_engine/tiling/TilesetTileRegistry.h"

#include <memory>

using namespace earth_engine;

namespace {

struct GeographicRootFixture {
    TilesetTileRegistry registry;
    std::unique_ptr<TileScheme> scheme = TileScheme::createGeographicTMS();
    TileContentLifecycleManager lifecycle;
    TileContentAccess contentAccess{
        registry,
        *scheme,
        nullptr,
        nullptr,
        lifecycle,
        2};
};

struct WebMercatorRootFixture {
    TilesetTileRegistry registry;
    std::unique_ptr<TileScheme> scheme = TileScheme::createXYZWebMercator();
    TileContentLifecycleManager lifecycle;
    TileContentAccess contentAccess{
        registry,
        *scheme,
        nullptr,
        nullptr,
        lifecycle,
        1};
};

} // namespace

TEST(TileSelectionRootPolicyTest, ExplicitContentRootsTakePriority) {
    const std::vector<TileKey> explicitRoots{
        TileKey{"content", 2, 4, 6},
        TileKey{"content", 3, 5, 7},
    };

    EXPECT_EQ(
        TileSelectionRootPolicy::chooseRoots(
            "Geographic-TMS",
            explicitRoots,
            true),
        explicitRoots);
}

TEST(TileSelectionRootPolicyTest, GeographicTmsUsesVirtualTerrainRoot) {
    const std::vector<TileKey> roots =
        TileSelectionRootPolicy::chooseRoots("Geographic-TMS", {}, true);

    ASSERT_EQ(roots.size(), 1u);
    EXPECT_EQ(
        roots[0],
        TileSelectionRootPolicy::virtualTerrainRootKey("Geographic-TMS"));
    EXPECT_TRUE(TileSelectionRootPolicy::isVirtualTerrainRoot(roots[0]));
}

TEST(TileSelectionRootPolicyTest, GeographicTmsWithoutTerrainDomainUsesDataRoots) {
    const std::vector<TileKey> roots =
        TileSelectionRootPolicy::chooseRoots("Geographic-TMS", {}, false);

    ASSERT_EQ(roots.size(), 2u);
    EXPECT_EQ(roots[0], (TileKey{"Geographic-TMS", 0, 0, 0}));
    EXPECT_EQ(roots[1], (TileKey{"Geographic-TMS", 0, 1, 0}));
}

TEST(TileSelectionRootPolicyTest, GeographicTmsLevelZeroDataRootsStayExplicit) {
    const std::vector<TileKey> roots =
        TileSelectionRootPolicy::levelZeroTerrainRoots("Geographic-TMS");

    ASSERT_EQ(roots.size(), 2u);
    EXPECT_EQ(roots[0], (TileKey{"Geographic-TMS", 0, 0, 0}));
    EXPECT_EQ(roots[1], (TileKey{"Geographic-TMS", 0, 1, 0}));
}

TEST(TileSelectionRootPolicyTest, WebMercatorUsesVirtualTerrainRoot) {
    const std::vector<TileKey> roots =
        TileSelectionRootPolicy::chooseRoots("XYZ-WebMercator", {}, true);

    ASSERT_EQ(roots.size(), 1u);
    EXPECT_EQ(
        roots[0],
        TileSelectionRootPolicy::virtualTerrainRootKey("XYZ-WebMercator"));
    EXPECT_TRUE(TileSelectionRootPolicy::isVirtualTerrainRoot(roots[0]));
}

TEST(TileSelectionRootPolicyTest, WebMercatorWithoutTerrainDomainUsesDataRoot) {
    const std::vector<TileKey> roots =
        TileSelectionRootPolicy::chooseRoots("XYZ-WebMercator", {}, false);

    ASSERT_EQ(roots.size(), 1u);
    EXPECT_EQ(roots[0], (TileKey{"XYZ-WebMercator", 0, 0, 0}));
    EXPECT_FALSE(TileSelectionRootPolicy::isVirtualTerrainRoot(roots[0]));
}

TEST(TileSelectionRootPolicyTest, OpenGlobusEarthUsesThreeRoots) {
    const std::vector<TileKey> roots =
        TileSelectionRootPolicy::chooseRoots("OpenGlobus-Earth", {}, true);

    ASSERT_EQ(roots.size(), 3u);
    EXPECT_EQ(roots[0], (TileKey{"OpenGlobus-Earth", 0, 0, 0}));
    EXPECT_EQ(roots[1], (TileKey{"OpenGlobus-Earth", 0, 0, 1}));
    EXPECT_EQ(roots[2], (TileKey{"OpenGlobus-Earth", 0, 0, 2}));
}

TEST(TileSelectionRootPolicyTest, UnknownSchemeUsesOneDefaultRoot) {
    const std::vector<TileKey> roots =
        TileSelectionRootPolicy::chooseRoots("custom", {}, true);

    ASSERT_EQ(roots.size(), 1u);
    EXPECT_EQ(roots[0], (TileKey{"custom", 0, 0, 0}));
}

TEST(TileSelectionRootPolicyTest, VirtualTerrainRootIsEmptyDoneRefineNode) {
    GeographicRootFixture fixture;
    const TileKey rootKey =
        TileSelectionRootPolicy::virtualTerrainRootKey("Geographic-TMS");

    TilesetTile* root = fixture.contentAccess.ensureTile(rootKey);

    ASSERT_NE(root, nullptr);
    EXPECT_EQ(root->key, rootKey);
    EXPECT_EQ(root->bounds, Rectangle::MAXIMUM);
    EXPECT_EQ(root->content.loadState, TileLoadState::Done);
    EXPECT_EQ(root->content.contentKind, TileContentKind::Empty);
    EXPECT_TRUE(root->unconditionallyRefine);
    ASSERT_TRUE(root->boundingVolume.has_value());
    EXPECT_EQ(root->boundingVolume->kind, TileBoundingVolumeKind::Region);
    EXPECT_DOUBLE_EQ(root->boundingVolume->minimumHeight, -1000.0);
    EXPECT_DOUBLE_EQ(root->boundingVolume->maximumHeight, 9000.0);
    EXPECT_EQ(root->rasterOverlayState.mappings().size(), 2u);
}

TEST(TileSelectionRootPolicyTest, VirtualGeographicRootLinksLevelZeroDataTiles) {
    GeographicRootFixture fixture;
    TilesetTile* root = fixture.contentAccess.ensureTile(
        TileSelectionRootPolicy::virtualTerrainRootKey("Geographic-TMS"));

    fixture.contentAccess.ensureTileChildren(*root);
    fixture.contentAccess.ensureTileChildren(*root);

    ASSERT_EQ(root->children.size(), 2u);
    EXPECT_EQ(root->children[0]->key, (TileKey{"Geographic-TMS", 0, 0, 0}));
    EXPECT_EQ(root->children[1]->key, (TileKey{"Geographic-TMS", 0, 1, 0}));
    EXPECT_EQ(root->children[0]->parent, root);
    EXPECT_EQ(root->children[1]->parent, root);
    EXPECT_EQ(root->children[0]->content.loadState, TileLoadState::Unloaded);
    EXPECT_EQ(root->children[1]->content.loadState, TileLoadState::Unloaded);
    EXPECT_FALSE(root->children[0]->unconditionallyRefine);
    EXPECT_FALSE(root->children[1]->unconditionallyRefine);
    EXPECT_GT(root->children[0]->geometricError, 0.0);
    EXPECT_GT(root->children[1]->geometricError, 0.0);
}

TEST(TileSelectionRootPolicyTest, VirtualWebMercatorRootLinksLevelZeroDataTile) {
    WebMercatorRootFixture fixture;
    TilesetTile* root = fixture.contentAccess.ensureTile(
        TileSelectionRootPolicy::virtualTerrainRootKey("XYZ-WebMercator"));

    fixture.contentAccess.ensureTileChildren(*root);
    fixture.contentAccess.ensureTileChildren(*root);

    ASSERT_EQ(root->children.size(), 1u);
    EXPECT_EQ(root->children[0]->key, (TileKey{"XYZ-WebMercator", 0, 0, 0}));
    EXPECT_EQ(root->children[0]->parent, root);
    EXPECT_EQ(root->children[0]->content.loadState, TileLoadState::Unloaded);
    EXPECT_FALSE(root->children[0]->unconditionallyRefine);
    EXPECT_GT(root->children[0]->geometricError, 0.0);
    EXPECT_EQ(root->rasterOverlayState.mappings().size(), 1u);
}
