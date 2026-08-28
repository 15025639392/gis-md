#include <gtest/gtest.h>

#include "earth_engine/core/resources/FrameResourceBudget.h"
#include "earth_engine/tiling/DirectRasterMapping.h"
#include "earth_engine/tiling/TileContentAccess.h"
#include "earth_engine/tiling/TileContentLifecycleManager.h"
#include "earth_engine/tiling/TileOcclusionState.h"
#include "earth_engine/tiling/TileScheme.h"
#include "earth_engine/tiling/TileSelectionTraversalContextBuilder.h"
#include "../../helpers/RasterOverlayTestFrame.h"
#include "earth_engine/tiling/Tileset.h"
#include "earth_engine/tiling/TilesetTile.h"
#include "earth_engine/tiling/TilesetTileRegistry.h"

using namespace earth_engine;

namespace {

struct TraversalContextFixture {
    TilePlan tilePlan;
    TileLoadQueue loadQueue;
    TileSelectionCounters counters;
    TilesetOptions options;
    FrameResourceBudget budget;
    TilesetTileRegistry registry;
    std::unique_ptr<TileScheme> scheme = TileScheme::createGeographicTMS();
    TileContentLifecycleManager lifecycle;
    TileContentAccess contentAccess =
        TileContentAccess::forNoTerrain(
            registry,
            *scheme,
            nullptr);
    TileSelectionTraversalContextBinding binding{};

    TileSelectionTraversalContext build() {
        return TileSelectionTraversalContextBuilder::build(
            TileSelectionTraversalContextBuildInput{
                tilePlan,
                loadQueue,
                counters,
                options,
                nullptr,
                nullptr,
                budget,
                Vec3(1.0, 2.0, 3.0),
                contentAccess,
                nullptr,
                earth_engine::testing::emptyRasterOverlayFrame()},
            binding);
    }
};

struct VisitProbe {
    int calls = 0;
    TilesetTile* lastTile = nullptr;
    void* lastUserData = nullptr;
};

void probeOnVisitTile(void* userData, TilesetTile& tile) {
    auto* probe = static_cast<VisitProbe*>(userData);
    ++probe->calls;
    probe->lastTile = &tile;
    probe->lastUserData = userData;
}

} // namespace

// The builder now copies plain per-frame data (refs to the tileset's plan,
// queue, content access, ...) straight into the context — no type erasure. This
// verifies those references point back at the exact input objects.
TEST(TileSelectionTraversalContextBuilderTest, WiresContextToInputObjects) {
    TraversalContextFixture fixture;
    const TileSelectionTraversalContext context = fixture.build();

    EXPECT_EQ(&context.tilePlan, &fixture.tilePlan);
    EXPECT_EQ(&context.loadQueue, &fixture.loadQueue);
    EXPECT_EQ(&context.counters, &fixture.counters);
    EXPECT_EQ(&context.options, &fixture.options);
    EXPECT_EQ(&context.contentAccess, &fixture.contentAccess);
    EXPECT_EQ(context.device, nullptr);
    EXPECT_EQ(&context.frameResourceBudget, &fixture.budget);
    EXPECT_EQ(
        &context.rasterFrame,
        &earth_engine::testing::emptyRasterOverlayFrame());
}

// Occlusion is a genuinely injected policy (software occlusion vs. none): the
// binding carries the function pointer + user data, and the context dispatches
// through them.
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

// onVisitTile is the other injected policy: the live path registers into the
// tileset's active-set, the shadow path resets into the shadow tree. The
// builder must route the binding's hook + user data through to the context.
TEST(
    TileSelectionTraversalContextBuilderTest,
    OnVisitTileCallbackUsesBindingUserData) {
    TraversalContextFixture fixture;
    VisitProbe probe;
    fixture.binding.onVisitTile = probeOnVisitTile;
    fixture.binding.onVisitTileUserData = &probe;
    const TileSelectionTraversalContext context = fixture.build();

    TilesetTile tile(
        TileKey{"test", 0, 0, 0},
        Rectangle{-1.0, -1.0, 1.0, 1.0});
    context.onVisitTile(tile);

    EXPECT_EQ(probe.calls, 1);
    EXPECT_EQ(probe.lastTile, &tile);
    EXPECT_EQ(probe.lastUserData, &probe);
}

// A null onVisitTile hook (e.g. a path that opts out) must be a safe no-op
// rather than a null-pointer call.
TEST(TileSelectionTraversalContextBuilderTest, OnVisitTileNullHookIsNoOp) {
    TraversalContextFixture fixture;
    const TileSelectionTraversalContext context = fixture.build();

    TilesetTile tile(
        TileKey{"test", 0, 0, 0},
        Rectangle{-1.0, -1.0, 1.0, 1.0});
    EXPECT_NO_FATAL_FAILURE(context.onVisitTile(tile));
}
