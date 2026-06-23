#include <gtest/gtest.h>

#include "earth_engine/tiling/TileCacheKey.h"
#include "earth_engine/tiling/TileLodTransitionController.h"
#include "earth_engine/tiling/TileLodTransitionFrameUpdater.h"
#include "earth_engine/tiling/TileRenderPlanFrameRefresher.h"
#include "earth_engine/tiling/TileSelectionPlanAppender.h"
#include "earth_engine/tiling/TileScheme.h"
#include "earth_engine/tiling/Tileset.h"

#include <cmath>
#include <limits>
#include <memory>

using namespace earth_engine;

namespace earth_engine {
struct TilesetTestAccess {
    static TilesetTile* ensureTile(Tileset& tileset, const TileKey& key) {
        return tileset.contentAccess_.ensureTile(key);
    }

    static std::string terrainCacheKey(const TileKey& key) {
        return TileCacheKey::forTile(key);
    }

    static void putTerrainCache(
        Tileset& tileset,
        const TileKey& key,
        std::unique_ptr<DecodedHeightmap> heightmap) {
        tileset.contentLifecycle_.heightmapTerrainCache()[terrainCacheKey(key)] =
            std::move(heightmap);
    }

    static void ensureTileMesh(Tileset& tileset, TilesetTile& tile) {
        tileset.meshPreparation_.ensureTileMesh(tile);
    }

    static void beginTilePlan(Tileset& tileset) {
        tileset.tilePlan_ = TilePlan{};
    }

    static const TilePlan& tilePlan(Tileset& tileset) {
        return tileset.tilePlan_;
    }

    static size_t fadingOutSetSize(const Tileset& tileset) {
        return tileset.tilesFadingOut_.size();
    }

    static void addTileToCurrentPlan(Tileset& tileset, TilesetTile& tile) {
        TileSelectionPlanAppender::addTileToCurrentPlan(
            tileset.tilePlan_,
            tileset.loadQueue_,
            tileset.options_.enableLodTransitionPeriod,
            tile,
            1.0,
            true,
            std::numeric_limits<double>::max());
        refreshTilePlanRenderEntries(tileset);
    }

    static void updateLodTransitions(
        Tileset& tileset,
        double deltaSeconds) {
        TileLodTransitionFrameUpdater::update(
            tileset.tilePlan_,
            tileset.tileRegistry_,
            tileset.tilesFadingOut_,
            tileset.rasterOverlays_,
            deltaSeconds,
            TileLodTransitionFrameOptions{
                tileset.options_.enableLodTransitionPeriod,
                tileset.options_.lodTransitionLength});
        refreshTilePlanRenderEntries(tileset);
    }

private:
    static void refreshTilePlanRenderEntries(Tileset& tileset) {
        TileRenderPlanFrameRefresher::refresh(
            tileset.tilePlan_,
            tileset.contentAccess_,
            tileset.rasterOverlays_,
            TileRenderPlanFrameRefreshOptions{
                tileset.options_.enableLodTransitionPeriod,
                tileset.interactionActiveForFrame_,
                tileset.resourceSmoothingActiveForFrame_});
    }
};
} // namespace earth_engine

namespace {

std::unique_ptr<DecodedHeightmap> makeFlatHeightmap(float heightMeters) {
    auto heightmap = std::make_unique<DecodedHeightmap>();
    heightmap->tileSize = 2;
    heightmap->heights = {
        heightMeters,
        heightMeters,
        heightMeters,
        heightMeters};
    heightmap->minHeight = heightMeters;
    heightmap->maxHeight = heightMeters;
    return heightmap;
}

Tileset makeTransitionTileset() {
    TilesetOptions options;
    options.enableLodTransitionPeriod = true;
    options.lodTransitionLength = 1.0f;

    return Tileset(
        TileScheme::createGeographicTMS(),
        {},
        nullptr,
        options);
}

std::string testCacheKey(const TileKey& key) {
    return key.schemeId + ":" +
        std::to_string(key.z) + ":" +
        std::to_string(key.x) + ":" +
        std::to_string(key.y);
}

bool hasRenderTransitionContent(const TilesetTile& tile) {
    return tile.content.contentKind == TileContentKind::Render;
}

} // namespace

TEST(TileLodTransitionsTest, ControllerFadesOutPreviousRenderContent) {
    const TileKey rootKey{"test", 0, 0, 0};
    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;
    tiles.emplace(
        testCacheKey(rootKey),
        std::make_unique<TilesetTile>(rootKey, Rectangle{}));
    TilesetTile& root = *tiles[testCacheKey(rootKey)];
    root.selectionFrameState.previousSelectionState =
        TileSelectionState::Rendered;
    root.content.contentKind = TileContentKind::Render;
    root.content.loadState = TileLoadState::Done;
    root.selectionFrameState.lodTransitionFadePercentage = 0.25f;

    TilePlan plan;
    std::unordered_set<std::string> fadingKeys;
    TileLodTransitionController::updateTransitions(
        plan,
        fadingKeys,
        0.25,
        TileLodTransitionOptions{&tiles, true, 1.0},
        testCacheKey,
        hasRenderTransitionContent);

    EXPECT_EQ(fadingKeys.count(testCacheKey(rootKey)), 1u);
    ASSERT_EQ(plan.tilesFadingOut.size(), 1u);
    EXPECT_LT(
        std::abs(
            root.selectionFrameState.lodTransitionFadePercentage - 0.25f),
        1e-6f);
    EXPECT_LT(std::abs(plan.tilesFadingOut.front().opacity - 0.75f), 1e-6f);
    EXPECT_EQ(plan.fadingNodeCount, 1);
}

TEST(TileLodTransitionsTest, ControllerRestartsReturnedFadeOutTile) {
    const TileKey rootKey{"test", 0, 0, 0};
    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;
    tiles.emplace(
        testCacheKey(rootKey),
        std::make_unique<TilesetTile>(rootKey, Rectangle{}));
    TilesetTile& root = *tiles[testCacheKey(rootKey)];
    root.selectionFrameState.previousSelectionState =
        TileSelectionState::Rendered;
    root.content.contentKind = TileContentKind::Render;
    root.content.loadState = TileLoadState::Done;
    root.selectionFrameState.lodTransitionFadePercentage = 0.75f;

    TilePlan plan;
    plan.visibleTiles.push_back(rootKey);
    std::unordered_set<std::string> fadingKeys{testCacheKey(rootKey)};
    TileLodTransitionController::updateTransitions(
        plan,
        fadingKeys,
        0.25,
        TileLodTransitionOptions{&tiles, true, 1.0},
        testCacheKey,
        hasRenderTransitionContent);

    EXPECT_TRUE(fadingKeys.empty());
    EXPECT_TRUE(plan.tilesFadingOut.empty());
    ASSERT_EQ(plan.tileTransitions.size(), 1u);
    EXPECT_LT(
        std::abs(
            root.selectionFrameState.lodTransitionFadePercentage - 0.25f),
        1e-6f);
    EXPECT_EQ(plan.fadingNodeCount, 1);
}

TEST(TileLodTransitionsTest, UsesNativeDeltaState) {
    Tileset tileset = makeTransitionTileset();

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    ASSERT_NE(root, nullptr);

    TilesetTestAccess::putTerrainCache(
        tileset,
        rootKey,
        makeFlatHeightmap(0.0f));
    TilesetTestAccess::ensureTileMesh(tileset, *root);

    TilesetTestAccess::beginTilePlan(tileset);
    root->selectionFrameState.previousSelectionState =
        TileSelectionState::NotVisited;
    TilesetTestAccess::addTileToCurrentPlan(tileset, *root);
    TilesetTestAccess::updateLodTransitions(tileset, 0.25);
    EXPECT_LT(
        std::abs(
            root->selectionFrameState.lodTransitionFadePercentage - 0.25f),
        1e-6f);
    EXPECT_EQ(TilesetTestAccess::tilePlan(tileset).fadingNodeCount, 1);

    TilesetTestAccess::beginTilePlan(tileset);
    root->selectionFrameState.selectionState = TileSelectionState::NotVisited;
    root->selectionFrameState.previousSelectionState =
        TileSelectionState::Rendered;
    TilesetTestAccess::updateLodTransitions(tileset, 0.25);
    const TilePlan& fadeOutPlan = TilesetTestAccess::tilePlan(tileset);
    EXPECT_TRUE(fadeOutPlan.visibleTiles.empty());
    ASSERT_EQ(fadeOutPlan.tilesFadingOut.size(), 1u);
    EXPECT_LT(std::abs(fadeOutPlan.tilesFadingOut.front().opacity - 0.75f),
              1e-6f);
    EXPECT_EQ(TilesetTestAccess::fadingOutSetSize(tileset), 1u);

    TilesetTestAccess::beginTilePlan(tileset);
    root->selectionFrameState.previousSelectionState =
        TileSelectionState::NotVisited;
    TilesetTestAccess::updateLodTransitions(tileset, 0.75);
    EXPECT_EQ(TilesetTestAccess::fadingOutSetSize(tileset), 1u);
    ASSERT_FALSE(TilesetTestAccess::tilePlan(tileset).tilesFadingOut.empty());
    EXPECT_LE(
        TilesetTestAccess::tilePlan(tileset).tilesFadingOut.front().opacity,
        0.001f);

    TilesetTestAccess::beginTilePlan(tileset);
    TilesetTestAccess::updateLodTransitions(tileset, 0.016);
    EXPECT_EQ(TilesetTestAccess::fadingOutSetSize(tileset), 0u);
}

TEST(TileLodTransitionsTest, AdditiveRefinedTileFadesOutAfterLeavingSelection) {
    Tileset tileset = makeTransitionTileset();

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    const TileKey childKey{"Geographic-TMS", 1, 0, 0};
    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    TilesetTile* child = TilesetTestAccess::ensureTile(tileset, childKey);
    ASSERT_NE(root, nullptr);
    ASSERT_NE(child, nullptr);

    TilesetTestAccess::putTerrainCache(
        tileset,
        rootKey,
        makeFlatHeightmap(0.0f));
    TilesetTestAccess::ensureTileMesh(tileset, *root);
    TilesetTestAccess::putTerrainCache(
        tileset,
        childKey,
        makeFlatHeightmap(0.0f));
    TilesetTestAccess::ensureTileMesh(tileset, *child);

    child->refine = TileRefine::Add;
    child->selectionFrameState.previousSelectionState =
        TileSelectionState::Refined;
    child->selectionFrameState.selectionState = TileSelectionState::NotVisited;

    TilesetTestAccess::beginTilePlan(tileset);
    TilesetTestAccess::addTileToCurrentPlan(tileset, *root);
    TilesetTestAccess::updateLodTransitions(tileset, 0.25);

    const TilePlan& plan = TilesetTestAccess::tilePlan(tileset);
    bool childFadingOut = false;
    float childOpacity = 0.0f;
    for (const TileTransition& transition : plan.tilesFadingOut) {
        if (transition.key == childKey) {
            childFadingOut = true;
            childOpacity = transition.opacity;
            break;
        }
    }

    EXPECT_TRUE(childFadingOut);
    EXPECT_LT(std::abs(childOpacity - 0.75f), 1e-6f);
}

TEST(TileLodTransitionsTest, EmptyContentDoesNotCreateFakeFade) {
    Tileset tileset = makeTransitionTileset();

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    ASSERT_NE(root, nullptr);

    root->content.loadState = TileLoadState::Done;
    root->content.contentKind = TileContentKind::Empty;
    root->content.renderContent.setMeshReady(false);

    TilesetTestAccess::beginTilePlan(tileset);
    root->selectionFrameState.previousSelectionState =
        TileSelectionState::NotVisited;
    TilesetTestAccess::addTileToCurrentPlan(tileset, *root);
    TilesetTestAccess::updateLodTransitions(tileset, 0.25);
    EXPECT_TRUE(TilesetTestAccess::tilePlan(tileset).tileTransitions.empty());
    EXPECT_EQ(TilesetTestAccess::tilePlan(tileset).fadingNodeCount, 0);
    EXPECT_LT(
        std::abs(
            root->selectionFrameState.lodTransitionFadePercentage - 1.0f),
        1e-6f);

    TilesetTestAccess::beginTilePlan(tileset);
    root->selectionFrameState.selectionState = TileSelectionState::NotVisited;
    root->selectionFrameState.previousSelectionState =
        TileSelectionState::Rendered;
    TilesetTestAccess::updateLodTransitions(tileset, 0.25);
    EXPECT_TRUE(TilesetTestAccess::tilePlan(tileset).tilesFadingOut.empty());
    EXPECT_EQ(TilesetTestAccess::fadingOutSetSize(tileset), 0u);
}
