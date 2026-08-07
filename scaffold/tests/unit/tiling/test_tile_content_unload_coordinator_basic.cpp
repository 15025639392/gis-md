#include <gtest/gtest.h>

#include "earth_engine/tiling/RasterMappedToTilesetTile.h"
#include "earth_engine/tiling/TileContentUnloadCoordinator.h"

#include <memory>
#include <string>
#include <unordered_map>

using namespace earth_engine;

namespace {

struct UnloadFixture {
    const std::string cacheKey = "test:0:0:0";
    std::unordered_map<std::string, std::unique_ptr<DecodedHeightmap>>
        terrainCache;
    TileEmptyContentRegistry emptyContentRegistry;
};

TileCacheUnloadContentResult unload(TilesetTile& tile, UnloadFixture& fixture) {
    return TileContentUnloadCoordinator::unloadContent(
        tile,
        fixture.cacheKey,
        fixture.terrainCache,
        fixture.emptyContentRegistry,
        nullptr);
}

} // namespace

TEST(TileContentUnloadCoordinatorBasicTest, EmptyContentClearsEmptyRegistry) {
    TilesetTile tile(TileKey{"test", 0, 0, 0}, Rectangle{});
    tile.content.contentKind = TileContentKind::Empty;
    tile.content.loadState = TileLoadState::Done;
    UnloadFixture fixture;
    fixture.emptyContentRegistry.insert(fixture.cacheKey);

    const TileCacheUnloadContentResult result = unload(tile, fixture);

    EXPECT_EQ(result, TileCacheUnloadContentResult::Remove);
    EXPECT_FALSE(fixture.emptyContentRegistry.contains(fixture.cacheKey));
    EXPECT_EQ(tile.content.contentKind, TileContentKind::Unknown);
    EXPECT_EQ(tile.content.loadState, TileLoadState::Unloaded);
}

TEST(TileContentUnloadCoordinatorBasicTest, LoadingContentKeepsCacheState) {
    TilesetTile tile(TileKey{"test", 0, 0, 0}, Rectangle{});
    tile.content.contentKind = TileContentKind::Unknown;
    tile.content.loadState = TileLoadState::ContentLoading;
    UnloadFixture fixture;
    fixture.terrainCache[fixture.cacheKey] =
        std::make_unique<DecodedHeightmap>();
    fixture.emptyContentRegistry.insert(fixture.cacheKey);

    const TileCacheUnloadContentResult result = unload(tile, fixture);

    EXPECT_EQ(result, TileCacheUnloadContentResult::Keep);
    EXPECT_EQ(tile.content.contentKind, TileContentKind::Unknown);
    EXPECT_EQ(tile.content.loadState, TileLoadState::ContentLoading);
    EXPECT_NE(fixture.terrainCache.find(fixture.cacheKey),
              fixture.terrainCache.end());
    EXPECT_TRUE(fixture.emptyContentRegistry.contains(fixture.cacheKey));
}

TEST(TileContentUnloadCoordinatorBasicTest, FailedUnknownClearsStaleEmptyMarker) {
    TilesetTile tile(TileKey{"test", 0, 0, 0}, Rectangle{});
    tile.content.contentKind = TileContentKind::Unknown;
    tile.content.loadState = TileLoadState::Failed;
    UnloadFixture fixture;
    fixture.emptyContentRegistry.insert(fixture.cacheKey);

    const TileCacheUnloadContentResult result = unload(tile, fixture);

    EXPECT_EQ(result, TileCacheUnloadContentResult::Remove);
    EXPECT_FALSE(fixture.emptyContentRegistry.contains(fixture.cacheKey));
    EXPECT_EQ(tile.content.contentKind, TileContentKind::Unknown);
    EXPECT_EQ(tile.content.loadState, TileLoadState::Unloaded);
}

TEST(
    TileContentUnloadCoordinatorBasicTest,
    ExternalContentClearsWrapperWhenUnreferenced) {
    TilesetTile tile(TileKey{"test", 0, 0, 0}, Rectangle{});
    tile.content.contentKind = TileContentKind::External;
    tile.content.loadState = TileLoadState::Done;
    tile.unconditionallyRefine = true;
    UnloadFixture fixture;
    fixture.emptyContentRegistry.insert(fixture.cacheKey);

    const TileCacheUnloadContentResult result = unload(tile, fixture);

    EXPECT_EQ(result, TileCacheUnloadContentResult::RemoveAndClearChildren);
    EXPECT_FALSE(fixture.emptyContentRegistry.contains(fixture.cacheKey));
    EXPECT_EQ(tile.content.contentKind, TileContentKind::Unknown);
    EXPECT_EQ(tile.content.loadState, TileLoadState::Unloaded);
    EXPECT_TRUE(tile.unconditionallyRefine);
}

TEST(
    TileContentUnloadCoordinatorBasicTest,
    ReferencedExternalContentKeepsCacheState) {
    TilesetTile tile(TileKey{"test", 0, 0, 0}, Rectangle{});
    tile.content.contentKind = TileContentKind::External;
    tile.content.loadState = TileLoadState::Done;
    tile.addReference();
    UnloadFixture fixture;
    fixture.emptyContentRegistry.insert(fixture.cacheKey);

    const TileCacheUnloadContentResult result = unload(tile, fixture);

    EXPECT_EQ(result, TileCacheUnloadContentResult::Keep);
    EXPECT_TRUE(fixture.emptyContentRegistry.contains(fixture.cacheKey));
    EXPECT_EQ(tile.content.contentKind, TileContentKind::External);
    EXPECT_EQ(tile.content.loadState, TileLoadState::Done);
    EXPECT_EQ(tile.referenceCount(), 1);
}

TEST(
    TileContentUnloadCoordinatorBasicTest,
    ReferencedExternalContentClearsRasterOverlayStateLikeCesiumNative) {
    TilesetTile tile(TileKey{"test", 0, 0, 0}, Rectangle{});
    tile.content.contentKind = TileContentKind::External;
    tile.content.loadState = TileLoadState::Done;
    tile.addReference();
    tile.rasterOverlayState.ensureMapping(0);
    tile.rasterOverlayState.missingProjections().push_back(
        RasterOverlayProjection::WebMercator);
    UnloadFixture fixture;
    fixture.emptyContentRegistry.insert(fixture.cacheKey);

    const TileCacheUnloadContentResult result = unload(tile, fixture);

    EXPECT_EQ(result, TileCacheUnloadContentResult::Keep);
    EXPECT_TRUE(fixture.emptyContentRegistry.contains(fixture.cacheKey));
    EXPECT_EQ(tile.content.contentKind, TileContentKind::External);
    EXPECT_EQ(tile.content.loadState, TileLoadState::Done);
    EXPECT_EQ(tile.referenceCount(), 1);
    EXPECT_EQ(tile.rasterOverlayState.mappingCount(), 0u);
    EXPECT_TRUE(tile.rasterOverlayState.missingProjections().empty());
}

TEST(
    TileContentUnloadCoordinatorBasicTest,
    ExternalContentWithReferencedDescendantClearsRasterOverlayStateBeforeKeep) {
    TilesetTile tile(TileKey{"test", 0, 0, 0}, Rectangle{});
    TilesetTile child(TileKey{"test", 1, 0, 0}, Rectangle{});
    tile.content.contentKind = TileContentKind::External;
    tile.content.loadState = TileLoadState::Done;
    tile.attachChild(child);
    child.addReference();
    tile.rasterOverlayState.ensureMapping(0);
    tile.rasterOverlayState.missingProjections().push_back(
        RasterOverlayProjection::WebMercator);
    UnloadFixture fixture;
    fixture.emptyContentRegistry.insert(fixture.cacheKey);

    const TileCacheUnloadContentResult result = unload(tile, fixture);

    EXPECT_EQ(result, TileCacheUnloadContentResult::Keep);
    EXPECT_TRUE(fixture.emptyContentRegistry.contains(fixture.cacheKey));
    EXPECT_EQ(tile.content.contentKind, TileContentKind::External);
    EXPECT_EQ(tile.content.loadState, TileLoadState::Done);
    EXPECT_EQ(child.referenceCount(), 1);
    EXPECT_EQ(tile.rasterOverlayState.mappingCount(), 0u);
    EXPECT_TRUE(tile.rasterOverlayState.missingProjections().empty());
}
