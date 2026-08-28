#include <gtest/gtest.h>

#include "earth_engine/tiling/DirectRasterMapping.h"
#include "earth_engine/tiling/TileUnloadPolicy.h"
#include "earth_engine/tiling/TileUnloadQueue.h"
#include "earth_engine/tiling/TilesetTile.h"

#include <memory>
#include <string>
#include <unordered_map>

using namespace earth_engine;

TEST(TileUnloadPolicyTest, ClassifiesContentUnloadQueueEligibility) {
    TilesetTile tile(TileKey{"test", 0, 0, 0}, Rectangle{});

    tile.content.contentKind = TileContentKind::Unknown;
    tile.content.loadState = TileLoadState::Unloaded;
    EXPECT_FALSE(TileUnloadPolicy::isEligibleForContentUnloadQueue(tile));

    tile.content.loadState = TileLoadState::Failed;
    EXPECT_TRUE(TileUnloadPolicy::isEligibleForContentUnloadQueue(tile));

    tile.content.contentKind = TileContentKind::Render;
    tile.content.loadState = TileLoadState::ContentLoading;
    EXPECT_FALSE(TileUnloadPolicy::isEligibleForContentUnloadQueue(tile));

    tile.content.loadState = TileLoadState::Unloading;
    EXPECT_FALSE(TileUnloadPolicy::isEligibleForContentUnloadQueue(tile));

    tile.content.loadState = TileLoadState::Done;
    EXPECT_TRUE(TileUnloadPolicy::isEligibleForContentUnloadQueue(tile));
}

TEST(TileUnloadPolicyTest, DefersReferencedSubtreesAndExternalActiveWork) {
    TilesetTile parent(TileKey{"test", 0, 0, 0}, Rectangle{});
    TilesetTile child(TileKey{"test", 1, 0, 0}, Rectangle{}, &parent);
    TilesetTile grandchild(TileKey{"test", 2, 0, 0}, Rectangle{}, &child);
    parent.children.push_back(&child);
    child.children.push_back(&grandchild);

    EXPECT_FALSE(TileUnloadPolicy::hasReferencedDescendant(parent));
    grandchild.addReference();
    EXPECT_TRUE(TileUnloadPolicy::hasReferencedDescendant(parent));
    EXPECT_TRUE(TileUnloadPolicy::shouldDeferForReferences(parent, false));
    grandchild.clearReferences();

    parent.content.contentKind = TileContentKind::External;
    EXPECT_TRUE(TileUnloadPolicy::shouldDeferForReferences(parent, true));

    parent.content.contentKind = TileContentKind::Render;
    EXPECT_FALSE(TileUnloadPolicy::shouldDeferForReferences(parent, true));
}

TEST(TileUnloadPolicyTest, ProtectsOnlyDirectLoadingUpsampledChildren) {
    TilesetTile parent(TileKey{"test", 0, 0, 0}, Rectangle{});
    TilesetTile child(TileKey{"test", 1, 0, 0}, Rectangle{}, &parent);
    TilesetTile grandchild(TileKey{"test", 2, 0, 0}, Rectangle{}, &child);
    parent.children.push_back(&child);

    child.content.markTerrainAvailabilityUpsample();
    child.content.loadState = TileLoadState::ContentLoading;
    EXPECT_TRUE(
        TileUnloadPolicy::hasContentLoadingUpsampledDirectChild(parent));

    child.content.clearUpsampleKind();
    child.content.loadState = TileLoadState::Done;
    child.children.push_back(&grandchild);
    grandchild.content.markTerrainAvailabilityUpsample();
    grandchild.content.loadState = TileLoadState::ContentLoading;
    EXPECT_FALSE(
        TileUnloadPolicy::hasContentLoadingUpsampledDirectChild(parent));
}

TEST(TileUnloadPolicyTest, FindsQueuedTilesByLoadState) {
    TileUnloadQueue queue;
    queue.pushBackIfAbsent("missing");
    queue.pushBackIfAbsent("null");
    queue.pushBackIfAbsent("loaded");
    queue.pushBackIfAbsent("unloading");

    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;
    tiles["null"] = nullptr;
    auto loaded = std::make_unique<TilesetTile>(
        TileKey{"test", 0, 0, 0},
        Rectangle{});
    loaded->content.loadState = TileLoadState::Done;
    tiles["loaded"] = std::move(loaded);
    auto unloading = std::make_unique<TilesetTile>(
        TileKey{"test", 0, 0, 1},
        Rectangle{});
    unloading->content.loadState = TileLoadState::Unloading;
    tiles["unloading"] = std::move(unloading);

    EXPECT_TRUE(TileUnloadPolicy::hasQueuedTileInState(
        queue,
        tiles,
        TileLoadState::Unloading));
    EXPECT_FALSE(TileUnloadPolicy::hasQueuedTileInState(
        queue,
        tiles,
        TileLoadState::Failed));
}
