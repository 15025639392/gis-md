#include <gtest/gtest.h>

#include "earth_engine/providers/DebugImageryProvider.h"
#include "earth_engine/providers/RasterOverlayTileProvider.h"
#include "earth_engine/renderer/Renderer.h"
#include "earth_engine/tiling/TileScheme.h"

#include <memory>

using namespace earth_engine;

namespace {

class TestTexture final : public Texture {
public:
    TestTexture(int width, int height) : width_(width), height_(height) {}

    int width() const override { return width_; }
    int height() const override { return height_; }

private:
    int width_ = 0;
    int height_ = 0;
};

} // namespace

TEST(RasterOverlayLifecycleTest, TrimRetainsAttachedTileUntilDetach) {
    DebugImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *scheme, nullptr);
    Renderer renderer(nullptr);

    TileKey key{scheme->id(), 3, 4, 2};

    provider.setFrameNumber(1);
    auto tile = provider.getTile(key);
    ASSERT_NE(tile, nullptr);
    tile->setTexture(std::make_unique<TestTexture>(64, 32));
    Texture* texture = tile->getTexture();
    ASSERT_NE(texture, nullptr);

    std::weak_ptr<RasterOverlayTile> weakTile = tile;
    renderer.attachRasterInMainThread(
        key,
        0,
        tile,
        texture,
        0.0f,
        0.0f,
        1.0f,
        1.0f);

    tile.reset();

    provider.setFrameNumber(200);
    provider.trimUnusedTiles();

    EXPECT_EQ(provider.getCachedTileCount(), 1);
    EXPECT_FALSE(weakTile.expired());

    const RasterAttachment* attachment = renderer.getAttachedRaster(key, 0);
    ASSERT_NE(attachment, nullptr);
    EXPECT_EQ(attachment->texture, texture);
    ASSERT_NE(attachment->tile, nullptr);
    EXPECT_EQ(attachment->tile.get(), weakTile.lock().get());

    renderer.detachRasterInMainThread(key, 0);
    provider.trimUnusedTiles();

    EXPECT_EQ(provider.getCachedTileCount(), 0);
    EXPECT_TRUE(weakTile.expired());
}
