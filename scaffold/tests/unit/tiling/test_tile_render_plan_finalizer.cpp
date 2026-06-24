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

std::unique_ptr<GltfModel> makeEmptyGltfModel() {
    return std::make_unique<GltfModel>();
}

void makeGltfRenderReady(TilesetTile& tile) {
    tile.content.renderContent.setGltfContent(makeEmptyGltfModel());
    tile.content.renderContent.setTerrainRenderContent(true);
    tile.content.renderContent.addGltfPrimitiveResource(
        GltfPrimitiveRenderResources{});
    tile.markRenderContentDone();
}

bool isDrawableRenderContent(const TilesetTile& tile) {
    return tile.hasSurfaceDrawable() ||
           tile.content.renderContent.isGltfRenderReady();
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

TEST(
    TileRenderPlanFinalizerTest,
    UsesReadyGltfAncestorFallbackWithoutSurfaceGeometry) {
    const TileKey parentKey{"test", 0, 0, 0};
    const TileKey childKey{"test", 1, 1, 0};
    TilesetTile parent(parentKey, Rectangle{0.0, 0.0, 2.0, 2.0});
    TilesetTile child(childKey, Rectangle{1.0, 1.0, 2.0, 2.0}, &parent);
    makeGltfRenderReady(parent);

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
            return isDrawableRenderContent(tile);
        });

    ASSERT_EQ(plan.renderEntries.size(), 1u);
    const TileRenderEntry& entry = plan.renderEntries.front();
    EXPECT_EQ(entry.selectedKey, childKey);
    EXPECT_EQ(entry.renderKey, parentKey);
    EXPECT_EQ(entry.reason, TileRenderEntryReason::AncestorFallback);
    EXPECT_TRUE(entry.usesAncestorFallback);
    EXPECT_TRUE(entry.surfaceClipEnabled);
    EXPECT_TRUE(entry.allowSynchronousMeshPrep);
    EXPECT_EQ(plan.renderEntryAncestorFallbackCount, 1);
    EXPECT_EQ(plan.renderEntrySynchronousPrepCount, 0);
    EXPECT_EQ(plan.renderEntryDeferredPrepCount, 0);
}

TEST(
    TileRenderPlanFinalizerTest,
    UsesReadyGltfAncestorWhileSelectedGltfResourcesArePending) {
    const TileKey parentKey{"test", 0, 0, 0};
    const TileKey childKey{"test", 1, 1, 0};
    TilesetTile parent(parentKey, Rectangle{0.0, 0.0, 2.0, 2.0});
    TilesetTile child(childKey, Rectangle{1.0, 1.0, 2.0, 2.0}, &parent);
    makeGltfRenderReady(parent);
    child.content.renderContent.setGltfContent(makeEmptyGltfModel());
    child.content.loadState = TileLoadState::Done;
    child.content.contentKind = TileContentKind::Render;
    ASSERT_TRUE(child.content.renderContent.hasGltfContent());
    ASSERT_FALSE(child.content.renderContent.isGltfRenderReady());

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
            return isDrawableRenderContent(tile);
        });

    ASSERT_EQ(plan.renderEntries.size(), 1u);
    const TileRenderEntry& entry = plan.renderEntries.front();
    EXPECT_EQ(entry.selectedKey, childKey);
    EXPECT_EQ(entry.renderKey, parentKey);
    EXPECT_EQ(entry.reason, TileRenderEntryReason::AncestorFallback);
    EXPECT_TRUE(entry.usesAncestorFallback);
    EXPECT_TRUE(entry.surfaceClipEnabled);
    EXPECT_TRUE(entry.allowSynchronousMeshPrep);
    EXPECT_EQ(plan.renderEntryAncestorFallbackCount, 1);
    EXPECT_EQ(plan.renderEntrySynchronousPrepCount, 0);
    EXPECT_EQ(plan.renderEntryDeferredPrepCount, 0);
}

TEST(
    TileRenderPlanFinalizerTest,
    RejectsGltfAncestorFallbackUntilResourcesAreReady) {
    const TileKey parentKey{"test", 0, 0, 0};
    const TileKey childKey{"test", 1, 1, 0};
    TilesetTile parent(parentKey, Rectangle{0.0, 0.0, 2.0, 2.0});
    TilesetTile child(childKey, Rectangle{1.0, 1.0, 2.0, 2.0}, &parent);
    parent.content.renderContent.setGltfContent(makeEmptyGltfModel());
    parent.content.loadState = TileLoadState::Done;
    parent.content.contentKind = TileContentKind::Render;

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
            return isDrawableRenderContent(tile);
        });

    ASSERT_EQ(plan.renderEntries.size(), 1u);
    const TileRenderEntry& entry = plan.renderEntries.front();
    EXPECT_EQ(entry.selectedKey, childKey);
    EXPECT_EQ(entry.renderKey, childKey);
    EXPECT_EQ(entry.reason, TileRenderEntryReason::SynchronousPrep);
    EXPECT_FALSE(entry.usesAncestorFallback);
    EXPECT_EQ(plan.renderEntryAncestorFallbackCount, 0);
    EXPECT_EQ(plan.renderEntrySynchronousPrepCount, 1);
    EXPECT_EQ(plan.renderEntryDeferredPrepCount, 0);
}

TEST(
    TileRenderPlanFinalizerTest,
    ContentProviderTerrainSurfaceResidueDoesNotBuildDirectEntry) {
    const TileKey rootKey{"test", 0, 0, 0};
    TilesetTile root(rootKey, Rectangle{});
    root.contentProviderTerrainQuadtreeTile = true;
    root.markRenderContentDone();
    root.content.renderContent.setSurfaceGpuBuffers(
        std::make_unique<DummyBuffer>(4),
        nullptr);
    ASSERT_TRUE(root.hasSurfaceDrawable());
    ASSERT_FALSE(root.content.renderContent.hasGltfContent());

    TilePlan plan;
    plan.visibleTiles.push_back(rootKey);
    TileRenderPlanFinalizer::refreshRenderEntries(
        plan,
        TileRenderPlanFinalizeOptions{
            false,
            true,
            0,
            1},
        [&root](const TileKey& key) -> TilesetTile* {
            return key == root.key ? &root : nullptr;
        },
        [](const TileKey& key) {
            return TileCacheKey::forTile(key);
        },
        [](const TilesetTile& tile) {
            return tile.hasSurfaceDrawable();
        });

    ASSERT_EQ(plan.renderEntries.size(), 1u);
    const TileRenderEntry& entry = plan.renderEntries.front();
    EXPECT_EQ(entry.selectedKey, rootKey);
    EXPECT_EQ(entry.renderKey, rootKey);
    EXPECT_EQ(entry.reason, TileRenderEntryReason::SynchronousPrep);
    EXPECT_TRUE(entry.allowSynchronousMeshPrep);
    EXPECT_EQ(plan.renderEntryAncestorFallbackCount, 0);
    EXPECT_EQ(plan.renderEntrySynchronousPrepCount, 1);
}

TEST(
    TileRenderPlanFinalizerTest,
    ContentProviderTerrainSurfaceResidueIsNotAncestorFallback) {
    const TileKey parentKey{"test", 0, 0, 0};
    const TileKey childKey{"test", 1, 1, 0};
    TilesetTile parent(parentKey, Rectangle{0.0, 0.0, 2.0, 2.0});
    TilesetTile child(childKey, Rectangle{1.0, 1.0, 2.0, 2.0}, &parent);
    parent.contentProviderTerrainQuadtreeTile = true;
    parent.markRenderContentDone();
    parent.content.renderContent.setSurfaceGpuBuffers(
        std::make_unique<DummyBuffer>(4),
        nullptr);
    ASSERT_TRUE(parent.hasSurfaceDrawable());
    ASSERT_FALSE(parent.content.renderContent.hasGltfContent());

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
    EXPECT_EQ(entry.renderKey, childKey);
    EXPECT_EQ(entry.reason, TileRenderEntryReason::SynchronousPrep);
    EXPECT_FALSE(entry.usesAncestorFallback);
    EXPECT_FALSE(entry.surfaceClipEnabled);
    EXPECT_EQ(plan.renderEntryAncestorFallbackCount, 0);
    EXPECT_EQ(plan.renderEntrySynchronousPrepCount, 1);
}

TEST(TileRenderPlanFinalizerTest, CountsRootPrepOnceToAvoidBlankFrame) {
    const TileKey rootKey{"test", 0, 0, 0};
    TilesetTile root(rootKey, Rectangle{});

    TilePlan plan;
    plan.visibleTiles.push_back(rootKey);
    TileRenderPlanFinalizer::refreshRenderEntries(
        plan,
        TileRenderPlanFinalizeOptions{
            false,
            true,
            0,
            1},
        [&root](const TileKey& key) -> TilesetTile* {
            return key == root.key ? &root : nullptr;
        },
        [](const TileKey& key) {
            return TileCacheKey::forTile(key);
        },
        [](const TilesetTile& tile) {
            return tile.hasSurfaceDrawable();
        });

    ASSERT_EQ(plan.renderEntries.size(), 1u);
    const TileRenderEntry& entry = plan.renderEntries.front();
    EXPECT_EQ(entry.renderKey, rootKey);
    EXPECT_EQ(entry.reason, TileRenderEntryReason::SynchronousPrep);
    EXPECT_TRUE(entry.allowSynchronousMeshPrep);
    EXPECT_EQ(plan.renderEntrySynchronousPrepCount, 1);
    EXPECT_EQ(plan.renderEntryDeferredPrepCount, 0);
}

TEST(TileRenderPlanFinalizerTest, DefersFallbackPrepDuringInteraction) {
    const TileKey parentKey{"test", 0, 0, 0};
    const TileKey childKey{"test", 1, 0, 0};
    TilesetTile parent(parentKey, Rectangle{0.0, 0.0, 2.0, 2.0});
    TilesetTile child(childKey, Rectangle{0.0, 1.0, 1.0, 2.0}, &parent);

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
        [&parent](const TilesetTile& tile) {
            return tile.key == parent.key;
        });

    ASSERT_EQ(plan.renderEntries.size(), 1u);
    const TileRenderEntry& entry = plan.renderEntries.front();
    EXPECT_EQ(entry.selectedKey, childKey);
    EXPECT_EQ(entry.renderKey, parentKey);
    EXPECT_EQ(entry.reason, TileRenderEntryReason::AncestorFallback);
    EXPECT_TRUE(entry.usesAncestorFallback);
    EXPECT_FALSE(entry.allowSynchronousMeshPrep);
    EXPECT_EQ(plan.renderEntryAncestorFallbackCount, 1);
    EXPECT_EQ(plan.renderEntrySynchronousPrepCount, 0);
    EXPECT_EQ(plan.renderEntryDeferredPrepCount, 1);
}

TEST(
    TileRenderPlanFinalizerTest,
    ReadsSelectionFrameFadeWhenLodTransitionEnabled) {
    const TileKey rootKey{"test", 0, 0, 0};
    TilesetTile root(rootKey, Rectangle{});
    root.selectionFrameState.lodTransitionFadePercentage = 0.25f;

    TilePlan plan;
    plan.visibleTiles.push_back(rootKey);
    TileRenderPlanFinalizer::refreshRenderEntries(
        plan,
        TileRenderPlanFinalizeOptions{
            true,
            false,
            0,
            1},
        [&root](const TileKey& key) -> TilesetTile* {
            return key == root.key ? &root : nullptr;
        },
        [](const TileKey& key) {
            return TileCacheKey::forTile(key);
        },
        [](const TilesetTile& tile) {
            return tile.hasSurfaceDrawable();
        });

    ASSERT_EQ(plan.renderEntries.size(), 1u);
    EXPECT_EQ(
        plan.renderEntries.front().reason,
        TileRenderEntryReason::SynchronousPrep);
    EXPECT_NEAR(plan.renderEntries.front().opacity, 0.25f, 1e-6f);
}

TEST(TileRenderPlanFinalizerTest, FadingTilesBecomeFadePassEntries) {
    const TileKey fadingKey{"test", 0, 0, 0};
    TilesetTile fading(fadingKey, Rectangle{});
    fading.markRenderContentDone();
    fading.content.renderContent.setSurfaceGpuBuffers(
        std::make_unique<DummyBuffer>(4),
        nullptr);

    TilePlan plan;
    plan.tilesFadingOut.push_back(TileTransition{fadingKey, 0.4f, 1});
    TileRenderPlanFinalizer::refreshRenderEntries(
        plan,
        TileRenderPlanFinalizeOptions{
            true,
            false,
            0,
            1},
        [&fading](const TileKey& key) -> TilesetTile* {
            return key == fading.key ? &fading : nullptr;
        },
        [](const TileKey& key) {
            return TileCacheKey::forTile(key);
        },
        [](const TilesetTile& tile) {
            return tile.hasSurfaceDrawable();
        });

    ASSERT_EQ(plan.renderEntries.size(), 1u);
    const TileRenderEntry& entry = plan.renderEntries.front();
    EXPECT_EQ(entry.selectedKey, fadingKey);
    EXPECT_EQ(entry.renderKey, fadingKey);
    EXPECT_FALSE(entry.selectedThisFrame);
    EXPECT_EQ(entry.reason, TileRenderEntryReason::FadingOut);
    EXPECT_EQ(entry.renderPass(), TileRenderEntryPass::Fading);
    EXPECT_NEAR(entry.opacity, 0.4f, 1e-6f);
}
