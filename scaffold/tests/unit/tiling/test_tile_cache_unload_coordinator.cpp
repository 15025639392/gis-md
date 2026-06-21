#include <gtest/gtest.h>

#include "earth_engine/tiling/RasterMappedToTilesetTile.h"
#include "earth_engine/tiling/TileCacheUnloadCoordinator.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

using namespace earth_engine;

namespace {

std::unique_ptr<TilesetTile> makeUnloadableTile(int y) {
    auto tile = std::make_unique<TilesetTile>(
        TileKey{"test", 0, 0, y},
        Rectangle{});
    tile->content.contentKind = TileContentKind::Render;
    tile->content.loadState = TileLoadState::Done;
    return tile;
}

} // namespace

TEST(TileCacheUnloadCoordinatorTest, RotatesReferencedCandidateAndUnloadsLaterTile) {
    TileUnloadQueue unloadQueue;
    unloadQueue.pushBackIfAbsent("referenced");
    unloadQueue.pushBackIfAbsent("free");

    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;
    auto referenced = makeUnloadableTile(0);
    referenced->addReference();
    tiles["referenced"] = std::move(referenced);
    tiles["free"] = makeUnloadableTile(1);

    std::vector<std::string> unloadedKeys;
    std::vector<std::string> markedIneligibleKeys;
    const TileCacheUnloadResult result = TileCacheUnloadCoordinator::run(
        unloadQueue,
        tiles,
        2,
        0,
        0.0,
        false,
        false,
        [](const TilesetTile&) {
            return false;
        },
        [&](TilesetTile& tile) {
            unloadedKeys.push_back(tile.key.y == 1 ? "free" : "referenced");
            return TileCacheUnloadContentResult::Remove;
        },
        [&](const std::string& key) {
            markedIneligibleKeys.push_back(key);
            unloadQueue.erase(key);
        },
        [](TilesetTile&) {});

    ASSERT_EQ(unloadedKeys.size(), 1u);
    EXPECT_EQ(unloadedKeys.front(), "free");
    ASSERT_EQ(markedIneligibleKeys.size(), 1u);
    EXPECT_EQ(markedIneligibleKeys.front(), "free");
    ASSERT_EQ(unloadQueue.size(), 1u);
    EXPECT_EQ(unloadQueue.front(), "referenced");
    EXPECT_TRUE(result.cacheBytesDirty);
    EXPECT_TRUE(result.shouldRefreshTotalBytes);
}

TEST(TileCacheUnloadCoordinatorTest, StopsAfterAllCandidatesAreDeferred) {
    TileUnloadQueue unloadQueue;
    unloadQueue.pushBackIfAbsent("a");
    unloadQueue.pushBackIfAbsent("b");

    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;
    for (int y = 0; y < 2; ++y) {
        auto tile = makeUnloadableTile(y);
        tile->addReference();
        tiles[y == 0 ? "a" : "b"] = std::move(tile);
    }

    int unloadAttempts = 0;
    int markedIneligibleCount = 0;
    const TileCacheUnloadResult result = TileCacheUnloadCoordinator::run(
        unloadQueue,
        tiles,
        2,
        0,
        0.0,
        false,
        false,
        [](const TilesetTile&) {
            return false;
        },
        [&](TilesetTile&) {
            ++unloadAttempts;
            return TileCacheUnloadContentResult::Remove;
        },
        [&](const std::string&) {
            ++markedIneligibleCount;
        },
        [](TilesetTile&) {});

    EXPECT_EQ(unloadAttempts, 0);
    EXPECT_EQ(markedIneligibleCount, 0);
    ASSERT_EQ(unloadQueue.size(), 2u);
    EXPECT_EQ(unloadQueue.front(), "a");
    EXPECT_FALSE(result.cacheBytesDirty);
    EXPECT_FALSE(result.shouldRefreshTotalBytes);
}

TEST(TileCacheUnloadCoordinatorTest, DropsMissingKeysAndContinues) {
    TileUnloadQueue unloadQueue;
    unloadQueue.pushBackIfAbsent("stale");
    unloadQueue.pushBackIfAbsent("free");

    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;
    tiles["free"] = makeUnloadableTile(1);

    std::vector<std::string> unloadedKeys;
    std::vector<std::string> markedIneligibleKeys;
    const TileCacheUnloadResult result = TileCacheUnloadCoordinator::run(
        unloadQueue,
        tiles,
        2,
        0,
        0.0,
        false,
        false,
        [](const TilesetTile&) {
            return false;
        },
        [&](TilesetTile& tile) {
            unloadedKeys.push_back(tile.key.y == 1 ? "free" : "stale");
            return TileCacheUnloadContentResult::Remove;
        },
        [&](const std::string& key) {
            markedIneligibleKeys.push_back(key);
            unloadQueue.erase(key);
        },
        [](TilesetTile&) {});

    ASSERT_EQ(unloadedKeys.size(), 1u);
    EXPECT_EQ(unloadedKeys.front(), "free");
    ASSERT_EQ(markedIneligibleKeys.size(), 1u);
    EXPECT_EQ(markedIneligibleKeys.front(), "free");
    EXPECT_TRUE(unloadQueue.empty());
    EXPECT_TRUE(result.cacheBytesDirty);
    EXPECT_TRUE(result.shouldRefreshTotalBytes);
}

TEST(TileCacheUnloadCoordinatorTest, DefersByteRefreshDuringResourceSmoothing) {
    TileUnloadQueue unloadQueue;
    unloadQueue.pushBackIfAbsent("free");

    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;
    tiles["free"] = makeUnloadableTile(0);

    int unloadAttempts = 0;
    const TileCacheUnloadResult result = TileCacheUnloadCoordinator::run(
        unloadQueue,
        tiles,
        1,
        0,
        0.0,
        true,
        false,
        [](const TilesetTile&) {
            return false;
        },
        [&](TilesetTile&) {
            ++unloadAttempts;
            return TileCacheUnloadContentResult::Remove;
        },
        [&](const std::string& key) {
            unloadQueue.erase(key);
        },
        [](TilesetTile&) {});

    EXPECT_EQ(unloadAttempts, 1);
    EXPECT_TRUE(result.cacheBytesDirty);
    EXPECT_FALSE(result.shouldRefreshTotalBytes);
}
