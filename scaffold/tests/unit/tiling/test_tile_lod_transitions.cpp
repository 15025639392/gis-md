#include <gtest/gtest.h>

#include "earth_engine/content/GltfModel.h"
#include "earth_engine/core/math/Mat4.h"
#include "earth_engine/core/math/Vec3.h"
#include "earth_engine/renderer/RenderDevice.h"
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

namespace {

class DummyBuffer final : public Buffer {
public:
    explicit DummyBuffer(size_t byteSize) : byteSize_(byteSize) {}
    size_t size() const override { return byteSize_; }
private:
    size_t byteSize_ = 0;
};

} // namespace

namespace earth_engine {
struct TilesetTestAccess {
    static TilesetTile* ensureTile(Tileset& tileset, const TileKey& key) {
        return tileset.contentAccess_.ensureTile(key);
    }

    static void ensureTileMesh(Tileset& /*tileset*/, TilesetTile& tile) {
        auto model = std::make_unique<GltfModel>();
        GltfPrimitive primitive;
        primitive.vertices.resize(4);
        primitive.vertices[0].positionEcef = Vec3(0.0, 0.0, 0.0);
        primitive.vertices[1].positionEcef = Vec3(1.0, 0.0, 0.0);
        primitive.vertices[2].positionEcef = Vec3(0.0, 1.0, 0.0);
        primitive.vertices[3].positionEcef = Vec3(1.0, 1.0, 0.0);
        primitive.indices = {0, 1, 2, 1, 3, 2};
        primitive.runtime.nodeIndex = 0;
        model->rasterOverlayDetails.setGeographicRectangle(tile.bounds);
        model->primitives.push_back(std::move(primitive));
        tile.content.renderContent.prepareGltfContent(
            std::move(model), Mat4::identity());
        tile.content.renderContent.setTerrainRenderContent(true);
        GltfPrimitiveRenderResources res;
        res.vertexBuffer = std::make_unique<DummyBuffer>(64);
        res.indexBuffer = std::make_unique<DummyBuffer>(12);
        res.indexCount = 6;
        res.vertexCount = 4;
        tile.content.renderContent.addGltfPrimitiveResource(std::move(res));
        tile.markRenderContentDone();
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
        // These tests drive selection state directly (no real traversal), so
        // feed the fade-out discovery loop the full registry as the active-set
        // — equivalent to the old full-registry scan the loop used to do.
        std::vector<TilesetTile*> activeTiles;
        for (auto& [ck, tile] : tileset.tileRegistry_.tiles()) {
            (void)ck;
            if (tile) activeTiles.push_back(tile.get());
        }
        const std::vector<TilesetTile*> previousActiveTiles;
        TileLodTransitionFrameUpdater::update(
            tileset.tilePlan_,
            tileset.tileRegistry_,
            activeTiles,
            previousActiveTiles,
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
    return key.schemeId.str() + ":" +
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
    const std::vector<TilesetTile*> activeTiles{&root};
    std::unordered_set<std::string> fadingKeys;
    TileLodTransitionController::updateTransitions(
        plan,
        fadingKeys,
        0.25,
        TileLodTransitionOptions{
            &tiles,
            &activeTiles,
            nullptr,
            true,
            1.0},
        testCacheKey,
        hasRenderTransitionContent);

    EXPECT_EQ(fadingKeys.count(testCacheKey(rootKey)), 1u);
    ASSERT_EQ(plan.tilesFadingOut.size(), 1u);
    EXPECT_LT(
        std::abs(
            root.selectionFrameState.lodTransitionFadePercentage - 0.25f),
        1e-6f);
    // Cross-fade 合成契约:outgoing 层恒不透明(opacity 1.0)当基底,fadePercentage
    // 计时器仍推进用于移除时机;不再是旧的 opacity=1-fade(那会导致中点透黑)。
    EXPECT_LT(std::abs(plan.tilesFadingOut.front().opacity - 1.0f), 1e-6f);
    EXPECT_EQ(plan.fadingNodeCount, 1);
}

TEST(
    TileLodTransitionsTest,
    ControllerDiscoversPreviousOnlyTraversalTileForFadeOut) {
    const TileKey parentKey{"test", 0, 0, 0};
    const TileKey childKey{"test", 1, 0, 0};
    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;
    tiles.emplace(
        testCacheKey(parentKey),
        std::make_unique<TilesetTile>(parentKey, Rectangle{}));
    tiles.emplace(
        testCacheKey(childKey),
        std::make_unique<TilesetTile>(childKey, Rectangle{}));
    TilesetTile& parent = *tiles[testCacheKey(parentKey)];
    TilesetTile& child = *tiles[testCacheKey(childKey)];
    parent.content.contentKind = TileContentKind::Render;
    parent.content.loadState = TileLoadState::Done;
    child.content.contentKind = TileContentKind::Render;
    child.content.loadState = TileLoadState::Done;
    child.selectionFrameState.previousSelectionState =
        TileSelectionState::Rendered;

    TilePlan plan;
    plan.visibleTiles.push_back(parentKey);
    const std::vector<TilesetTile*> currentActiveTiles{&parent};
    const std::vector<TilesetTile*> previousActiveTiles{&child};
    std::unordered_set<std::string> fadingKeys;

    TileLodTransitionController::updateTransitions(
        plan,
        fadingKeys,
        0.25,
        TileLodTransitionOptions{
            &tiles,
            &currentActiveTiles,
            &previousActiveTiles,
            true,
            1.0},
        testCacheKey,
        hasRenderTransitionContent);

    EXPECT_EQ(fadingKeys.count(testCacheKey(childKey)), 1u);
    ASSERT_EQ(plan.tilesFadingOut.size(), 1u);
    EXPECT_EQ(plan.tilesFadingOut.front().key, childKey);
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
    const std::vector<TilesetTile*> activeTiles{&root};
    std::unordered_set<std::string> fadingKeys{testCacheKey(rootKey)};
    TileLodTransitionController::updateTransitions(
        plan,
        fadingKeys,
        0.25,
        TileLodTransitionOptions{
            &tiles,
            &activeTiles,
            nullptr,
            true,
            1.0},
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
    // Cross-fade 合成契约:outgoing 恒不透明基底(opacity 1.0)。
    EXPECT_LT(std::abs(fadeOutPlan.tilesFadingOut.front().opacity - 1.0f),
              1e-6f);
    EXPECT_EQ(TilesetTestAccess::fadingOutSetSize(tileset), 1u);

    TilesetTestAccess::beginTilePlan(tileset);
    root->selectionFrameState.previousSelectionState =
        TileSelectionState::NotVisited;
    TilesetTestAccess::updateLodTransitions(tileset, 0.75);
    EXPECT_EQ(TilesetTestAccess::fadingOutSetSize(tileset), 1u);
    ASSERT_FALSE(TilesetTestAccess::tilePlan(tileset).tilesFadingOut.empty());
    // Cross-fade 合成契约:outgoing 层直到被移除前恒不透明基底(opacity 1.0),
    // 不再是旧的"淡到 opacity→0"(移除时机仍由 fadePercentage>=1 触发,见下一帧)。
    EXPECT_LT(
        std::abs(
            TilesetTestAccess::tilePlan(tileset)
                .tilesFadingOut.front()
                .opacity -
            1.0f),
        1e-6f);

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

    TilesetTestAccess::ensureTileMesh(tileset, *root);
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
    // Cross-fade 合成契约:outgoing 恒不透明基底(opacity 1.0)。
    EXPECT_LT(std::abs(childOpacity - 1.0f), 1e-6f);
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
