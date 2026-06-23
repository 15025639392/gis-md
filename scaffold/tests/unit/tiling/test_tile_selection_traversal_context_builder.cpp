#include <gtest/gtest.h>

#include "earth_engine/core/resources/FrameResourceBudget.h"
#include "earth_engine/tiling/RasterMappedToTilesetTile.h"
#include "earth_engine/tiling/TileContentAccess.h"
#include "earth_engine/tiling/TileContentLifecycleManager.h"
#include "earth_engine/tiling/TileOcclusionState.h"
#include "earth_engine/tiling/TileScheme.h"
#include "earth_engine/tiling/TileSelectionTraversalContextBuilder.h"
#include "earth_engine/tiling/Tileset.h"
#include "earth_engine/tiling/TilesetTileRegistry.h"

using namespace earth_engine;

namespace {

struct TraversalContextFixture {
    TilePlan tilePlan;
    TileLoadQueue loadQueue;
    TileSelectionCounters counters;
    TilesetOptions options;
    std::vector<ActivatedRasterOverlay*> rasterOverlays;
    FrameResourceBudget budget;
    TilesetTileRegistry registry;
    std::unique_ptr<TileScheme> scheme = TileScheme::createGeographicTMS();
    TileContentLifecycleManager lifecycle;
    TileContentAccess contentAccess =
        TileContentAccess::forLegacyTerrain(
        registry,
        *scheme,
        nullptr,
        nullptr,
        lifecycle.heightmapTerrainCache(),
        0);
    TileSelectionTraversalContextBinding binding{
        tilePlan,
        loadQueue,
        options,
        rasterOverlays,
        contentAccess};

    TileSelectionTraversalContext build() {
        return TileSelectionTraversalContextBuilder::build(
            TileSelectionTraversalContextBuildInput{
                tilePlan,
                loadQueue,
                counters,
                options,
                rasterOverlays,
                nullptr,
                budget,
                Vec3(1.0, 2.0, 3.0),
                contentAccess},
            binding);
    }
};

} // namespace

TEST(
    TileSelectionTraversalContextBuilderTest,
    QueueAndAddCallbacksWritePlanState) {
    TraversalContextFixture fixture;
    fixture.options.enableLodTransitionPeriod = false;
    TileSelectionTraversalContext context = fixture.build();

    TilesetTile tile(
        TileKey{"test", 0, 0, 0},
        Rectangle{-1.0, -1.0, 1.0, 1.0});

    context.queueTileLoad(tile.key, TileLoadPriorityGroup::Preload, 7.0);

    ASSERT_EQ(fixture.loadQueue.size(), 1u);
    EXPECT_EQ(fixture.loadQueue.front().key, tile.key);
    EXPECT_EQ(fixture.loadQueue.front().group, TileLoadPriorityGroup::Preload);
    EXPECT_EQ(fixture.loadQueue.front().priority, 7.0);

    context.addTileToCurrentPlan(tile, 4.0, true, 3.0);

    ASSERT_EQ(fixture.tilePlan.visibleTiles.size(), 1u);
    EXPECT_EQ(fixture.tilePlan.visibleTiles.front(), tile.key);
    EXPECT_EQ(
        tile.selectionFrameState.selectionState,
        TileSelectionState::Rendered);
    EXPECT_DOUBLE_EQ(tile.selectionFrameState.screenSpaceError, 4.0);
    EXPECT_FLOAT_EQ(tile.selectionFrameState.lodTransitionFadePercentage, 1.0f);
    ASSERT_EQ(fixture.loadQueue.size(), 1u);
    EXPECT_EQ(fixture.loadQueue.front().key, tile.key);
    EXPECT_EQ(
        fixture.loadQueue.front().group,
        TileLoadPriorityGroup::Normal);
    EXPECT_EQ(fixture.loadQueue.front().priority, 3.0);
}

TEST(
    TileSelectionTraversalContextBuilderTest,
    OcclusionCallbackUsesBindingUserData) {
    TraversalContextFixture fixture;
    int callbackCalls = 0;
    fixture.binding.occlusionUserData = &callbackCalls;
    fixture.binding.checkOcclusion =
        [](void* userData, const TilesetTile& tile) {
            auto* calls = static_cast<int*>(userData);
            ++(*calls);
            return tile.key.z == 0 ? TileOcclusionState::Occluded
                                   : TileOcclusionState::NotOccluded;
        };
    const TileSelectionTraversalContext context = fixture.build();

    const TilesetTile root(
        TileKey{"test", 0, 0, 0},
        Rectangle{-1.0, -1.0, 1.0, 1.0});
    const TilesetTile child(
        TileKey{"test", 1, 0, 0},
        Rectangle{-1.0, -1.0, 0.0, 0.0});

    EXPECT_EQ(context.checkOcclusion(root), TileOcclusionState::Occluded);
    EXPECT_EQ(
        context.checkOcclusion(child),
        TileOcclusionState::NotOccluded);
    EXPECT_EQ(callbackCalls, 2);
}
