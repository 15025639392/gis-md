#include <gtest/gtest.h>

#include "earth_engine/tiling/RasterMappedToTilesetTile.h"
#include "earth_engine/tiling/TileCacheKey.h"
#include "earth_engine/tiling/TileRenderReferenceReleaser.h"

#include <memory>
#include <string>
#include <unordered_map>

using namespace earth_engine;

namespace {

void addTile(
    std::unordered_map<std::string, std::unique_ptr<TilesetTile>>& tiles,
    const TileKey& key) {
    tiles.emplace(TileCacheKey::forTile(key), std::make_unique<TilesetTile>(
        key,
        Rectangle{}));
}

TilesetTile* findTile(
    const std::unordered_map<std::string, std::unique_ptr<TilesetTile>>& tiles,
    const TileKey& key) {
    const auto it = tiles.find(TileCacheKey::forTile(key));
    return it == tiles.end() ? nullptr : it->second.get();
}

} // namespace

TEST(TileRenderReferenceStateTest, CountsClampsClearsAndTracksLastUsedFrame) {
    TilesetTile tile(TileKey{"test", 0, 0, 0}, Rectangle{});

    EXPECT_EQ(tile.referenceCount(), 0);
    tile.addReference();
    tile.addReference();
    EXPECT_EQ(tile.referenceCount(), 2);

    tile.removeReference();
    EXPECT_EQ(tile.referenceCount(), 1);

    tile.removeReference();
    tile.removeReference();
    EXPECT_EQ(tile.referenceCount(), 0);

    tile.addReference();
    tile.clearReferences();
    EXPECT_EQ(tile.referenceCount(), 0);

    tile.markUsedForRenderFrame(42);
    EXPECT_EQ(tile.lastUsedFrame(), 42u);
}

TEST(TileRenderReferenceStateTest, ReleaserClearsAllRenderReferencesOnly) {
    const TileKey firstKey{"test", 0, 0, 0};
    const TileKey secondKey{"test", 0, 1, 0};
    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;
    addTile(tiles, firstKey);
    addTile(tiles, secondKey);

    TilesetTile* first = findTile(tiles, firstKey);
    TilesetTile* second = findTile(tiles, secondKey);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);

    first->addReference();
    first->addReference();
    first->markUsedForRenderFrame(7);
    second->addReference();
    second->markUsedForRenderFrame(9);

    TileRenderReferenceReleaser::release(tiles);

    EXPECT_EQ(first->referenceCount(), 0);
    EXPECT_EQ(second->referenceCount(), 0);
    EXPECT_EQ(first->lastUsedFrame(), 7u);
    EXPECT_EQ(second->lastUsedFrame(), 9u);
}
