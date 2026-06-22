#include <gtest/gtest.h>

#include "earth_engine/terrain/TerrainTile.h"
#include "earth_engine/tiling/RasterMappedToTilesetTile.h"
#include "earth_engine/tiling/TileContentUnloadCoordinator.h"

#include <memory>
#include <string>
#include <unordered_map>

using namespace earth_engine;

namespace {

struct ProtectedSourceFixture {
    TilesetTile parent{TileKey{"test", 0, 0, 0}, Rectangle{}};
    TilesetTile child{TileKey{"test", 1, 0, 0}, Rectangle{}, &parent};
    const std::string cacheKey = "test:0:0:0";
    std::unordered_map<std::string, std::unique_ptr<DecodedHeightmap>>
        terrainCache;
    TileEmptyContentRegistry emptyContentRegistry;

    ProtectedSourceFixture() {
        parent.children.push_back(&child);
        parent.content.contentKind = TileContentKind::Render;
        parent.content.loadState = TileLoadState::Done;
        parent.content.renderContent.setMeshReady(true);
        parent.content.renderContent.setSurfaceMesh(
            std::make_unique<SurfaceTileMesh>());
        parent.content.renderContent.setSurfaceDrawable(true);
        parent.content.renderContent.setSurfaceSource(
            SurfaceDrawableSource::OwnTerrain);
        parent.selectionFrameState.renderable = true;
        parent.selectionFrameState.completeRenderable = true;
        child.content.upsampledFromParent = true;
        child.content.loadState = TileLoadState::ContentLoading;
        terrainCache[cacheKey] = std::make_unique<DecodedHeightmap>();
    }

    TileCacheUnloadContentResult unloadParent() {
        return TileContentUnloadCoordinator::unloadContent(
            parent,
            cacheKey,
            terrainCache,
            emptyContentRegistry,
            nullptr);
    }
};

} // namespace

TEST(
    TileContentUnloadCoordinatorUpsampleTest,
    ProtectedUpsampleSourceReleasesMainThreadResourcesAndKeepsContent) {
    ProtectedSourceFixture fixture;

    const TileCacheUnloadContentResult firstResult = fixture.unloadParent();

    EXPECT_EQ(firstResult, TileCacheUnloadContentResult::Keep);
    EXPECT_EQ(fixture.parent.content.contentKind, TileContentKind::Render);
    EXPECT_EQ(fixture.parent.content.loadState, TileLoadState::Unloading);
    EXPECT_TRUE(fixture.parent.content.renderContent.hasSurfaceMesh());
    EXPECT_TRUE(fixture.parent.content.renderContent.isMeshReady());
    EXPECT_FALSE(fixture.parent.content.renderContent.isSurfaceDrawable());
    EXPECT_EQ(fixture.parent.content.renderContent.currentSurfaceSource(),
              SurfaceDrawableSource::None);
    EXPECT_FALSE(fixture.parent.selectionFrameState.renderable);
    EXPECT_FALSE(fixture.parent.selectionFrameState.completeRenderable);
    EXPECT_NE(fixture.terrainCache.find(fixture.cacheKey),
              fixture.terrainCache.end());

    const TileCacheUnloadContentResult secondResult = fixture.unloadParent();

    EXPECT_EQ(secondResult, TileCacheUnloadContentResult::Keep);
    EXPECT_EQ(fixture.parent.content.contentKind, TileContentKind::Render);
    EXPECT_EQ(fixture.parent.content.loadState, TileLoadState::Unloading);
    EXPECT_TRUE(fixture.parent.content.renderContent.hasSurfaceMesh());
    EXPECT_NE(fixture.terrainCache.find(fixture.cacheKey),
              fixture.terrainCache.end());
}

TEST(
    TileContentUnloadCoordinatorUpsampleTest,
    CompletedProtectedUpsampleSourceFinishesUnload) {
    ProtectedSourceFixture fixture;
    fixture.parent.content.loadState = TileLoadState::Unloading;
    fixture.child.content.loadState = TileLoadState::Done;

    const TileCacheUnloadContentResult result = fixture.unloadParent();

    EXPECT_EQ(result, TileCacheUnloadContentResult::Remove);
    EXPECT_EQ(fixture.parent.content.contentKind, TileContentKind::Unknown);
    EXPECT_EQ(fixture.parent.content.loadState, TileLoadState::Unloaded);
    EXPECT_FALSE(fixture.parent.content.renderContent.hasSurfaceMesh());
    EXPECT_FALSE(fixture.parent.content.renderContent.isMeshReady());
    EXPECT_EQ(fixture.terrainCache.find(fixture.cacheKey),
              fixture.terrainCache.end());
}

TEST(
    TileContentUnloadCoordinatorUpsampleTest,
    NestedProtectedUpsampleSourceKeepsAncestorContent) {
    ProtectedSourceFixture fixture;
    TilesetTile grandchild{
        TileKey{"test", 2, 0, 0},
        Rectangle{},
        &fixture.child};
    fixture.child.children.push_back(&grandchild);
    fixture.child.content.upsampledFromParent = false;
    fixture.child.content.loadState = TileLoadState::Done;
    grandchild.content.upsampledFromParent = true;
    grandchild.content.loadState = TileLoadState::ContentLoading;

    const TileCacheUnloadContentResult result = fixture.unloadParent();

    EXPECT_EQ(result, TileCacheUnloadContentResult::Keep);
    EXPECT_EQ(fixture.parent.content.contentKind, TileContentKind::Render);
    EXPECT_EQ(fixture.parent.content.loadState, TileLoadState::Unloading);
    EXPECT_TRUE(fixture.parent.content.renderContent.hasSurfaceMesh());
    EXPECT_FALSE(fixture.parent.content.renderContent.isSurfaceDrawable());
    EXPECT_NE(fixture.terrainCache.find(fixture.cacheKey),
              fixture.terrainCache.end());
}
