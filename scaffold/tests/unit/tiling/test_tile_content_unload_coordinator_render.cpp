#include <gtest/gtest.h>

#include "earth_engine/terrain/TerrainTile.h"
#include "earth_engine/tiling/RasterMappedToTilesetTile.h"
#include "earth_engine/tiling/TileContentUnloadCoordinator.h"

#include <memory>
#include <string>
#include <unordered_map>

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

TEST(
    TileContentUnloadCoordinatorRenderTest,
    RenderContentClearsTerrainCacheAndRenderResources) {
    TilesetTile tile(TileKey{"test", 0, 0, 0}, Rectangle{});
    tile.content.contentKind = TileContentKind::Render;
    tile.content.loadState = TileLoadState::Done;
    tile.content.renderContent.setMeshReady(true);
    tile.content.renderContent.setSurfaceDrawable(true);
    tile.content.renderContent.setSurfaceMesh(
        std::make_unique<SurfaceTileMesh>());
    tile.content.renderContent.setSurfaceGpuBuffers(
        std::make_unique<DummyBuffer>(4),
        nullptr);
    tile.selectionFrameState.updateFrameRenderability(true);

    const std::string cacheKey = "test:0:0:0";
    std::unordered_map<std::string, std::unique_ptr<DecodedHeightmap>>
        terrainCache;
    terrainCache[cacheKey] = std::make_unique<DecodedHeightmap>();
    TileEmptyContentRegistry emptyContentRegistry;
    emptyContentRegistry.insert(cacheKey);

    const TileCacheUnloadContentResult result =
        TileContentUnloadCoordinator::unloadContent(
            tile,
            cacheKey,
            terrainCache,
            emptyContentRegistry,
            nullptr);

    EXPECT_EQ(result, TileCacheUnloadContentResult::Remove);
    EXPECT_EQ(terrainCache.find(cacheKey), terrainCache.end());
    EXPECT_FALSE(emptyContentRegistry.contains(cacheKey));
    EXPECT_EQ(tile.content.contentKind, TileContentKind::Unknown);
    EXPECT_EQ(tile.content.loadState, TileLoadState::Unloaded);
    EXPECT_FALSE(tile.content.renderContent.hasSurfaceMesh());
    EXPECT_FALSE(tile.content.renderContent.isMeshReady());
    EXPECT_FALSE(tile.content.renderContent.isSurfaceDrawable());
    EXPECT_FALSE(tile.selectionFrameState.renderable);
    EXPECT_FALSE(tile.selectionFrameState.completeRenderable);
}
