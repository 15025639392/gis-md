#include <gtest/gtest.h>

#include "earth_engine/renderer/RenderDevice.h"
#include "earth_engine/tiling/RasterMappedToTilesetTile.h"
#include "earth_engine/tiling/TileCacheKey.h"
#include "earth_engine/tiling/TileRenderPlanFinalizer.h"

#include <memory>
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

TilesetTile* findTile(
    const std::unordered_map<std::string, TilesetTile*>& tiles,
    const TileKey& key) {
    const auto it = tiles.find(TileCacheKey::forTile(key));
    return it == tiles.end() ? nullptr : it->second;
}

} // namespace

TEST(
    TileRenderPlanFinalizerTest,
    UsesRenderableAncestorFallbackWithSurfaceClip) {
    const TileKey parentKey{"test", 0, 0, 0};
    const TileKey childKey{"test", 1, 1, 0};
    TilesetTile parent(parentKey, Rectangle{0.0, 0.0, 2.0, 2.0});
    TilesetTile child(childKey, Rectangle{1.0, 1.0, 2.0, 2.0}, &parent);
    parent.markRenderContentDone();
    parent.content.renderContent.setSurfaceGpuBuffers(
        std::make_unique<DummyBuffer>(4),
        nullptr);

    std::unordered_map<std::string, TilesetTile*> tiles{
        {TileCacheKey::forTile(parentKey), &parent},
        {TileCacheKey::forTile(childKey), &child}};

    TilePlan plan;
    plan.visibleTiles.push_back(childKey);
    TileRenderPlanFinalizer::refreshRenderEntries(
        plan,
        TileRenderPlanFinalizeOptions{
            false,
            true,
            0,
            1},
        [&tiles](const TileKey& key) {
            return findTile(tiles, key);
        },
        [](const TileKey& key) {
            return TileCacheKey::forTile(key);
        },
        [](const TilesetTile& tile) {
            return tile.hasSurfaceDrawable();
        });

    ASSERT_EQ(plan.renderEntries.size(), 1u);
    const TileRenderEntry& entry = plan.renderEntries.front();
    EXPECT_EQ(entry.selectedKey, childKey);
    EXPECT_EQ(entry.renderKey, parentKey);
    EXPECT_EQ(entry.reason, TileRenderEntryReason::AncestorFallback);
    EXPECT_TRUE(entry.usesAncestorFallback);
    EXPECT_TRUE(entry.surfaceClipEnabled);
    EXPECT_EQ(plan.renderEntryAncestorFallbackCount, 1);
    EXPECT_EQ(plan.renderEntrySynchronousPrepCount, 0);
    EXPECT_EQ(plan.renderEntryDeferredPrepCount, 0);
    EXPECT_NEAR(entry.surfaceClipUv[0], 0.5f, 1e-6f);
    EXPECT_NEAR(entry.surfaceClipUv[1], 0.0f, 1e-6f);
    EXPECT_NEAR(entry.surfaceClipUv[2], 0.5f, 1e-6f);
    EXPECT_NEAR(entry.surfaceClipUv[3], 0.5f, 1e-6f);
}

TEST(
    TileRenderPlanFinalizerTest,
    FullGeometryReplacesEarlierClippedFallback) {
    const TileKey parentKey{"test", 0, 0, 0};
    const TileKey childKey{"test", 1, 0, 0};
    TilesetTile parent(parentKey, Rectangle{0.0, 0.0, 2.0, 2.0});
    TilesetTile child(childKey, Rectangle{0.0, 1.0, 1.0, 2.0}, &parent);
    parent.markRenderContentDone();
    parent.content.renderContent.setSurfaceGpuBuffers(
        std::make_unique<DummyBuffer>(4),
        nullptr);

    std::unordered_map<std::string, TilesetTile*> tiles{
        {TileCacheKey::forTile(parentKey), &parent},
        {TileCacheKey::forTile(childKey), &child}};

    TilePlan plan;
    plan.visibleTiles.push_back(childKey);
    plan.visibleTiles.push_back(parentKey);
    TileRenderPlanFinalizer::refreshRenderEntries(
        plan,
        TileRenderPlanFinalizeOptions{
            false,
            false,
            0,
            1},
        [&tiles](const TileKey& key) {
            return findTile(tiles, key);
        },
        [](const TileKey& key) {
            return TileCacheKey::forTile(key);
        },
        [](const TilesetTile& tile) {
            return tile.hasSurfaceDrawable();
        });

    ASSERT_EQ(plan.renderEntries.size(), 1u);
    const TileRenderEntry& entry = plan.renderEntries.front();
    EXPECT_EQ(entry.selectedKey, parentKey);
    EXPECT_EQ(entry.renderKey, parentKey);
    EXPECT_EQ(entry.reason, TileRenderEntryReason::Direct);
    EXPECT_FALSE(entry.usesAncestorFallback);
    EXPECT_FALSE(entry.surfaceClipEnabled);
    EXPECT_EQ(plan.renderEntryAncestorFallbackCount, 0);
}
