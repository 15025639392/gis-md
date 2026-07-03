#include <gtest/gtest.h>

#include "earth_engine/content/GltfModel.h"
#include "earth_engine/core/math/Mat4.h"
#include "earth_engine/renderer/RenderDevice.h"
#include "earth_engine/tiling/RasterMappedToTilesetTile.h"
#include "earth_engine/tiling/TileCacheKey.h"
#include "earth_engine/tiling/TileRenderPlanFinalizer.h"

#include <array>
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

std::unique_ptr<GltfModel> makeQuadTerrainGltfModel(
    const Rectangle& rectangle) {
    auto model = std::make_unique<GltfModel>();
    const Vec3 nodeOrigin(100.0, 200.0, 300.0);
    GltfNodeRuntime rootNode;
    rootNode.baseLocalTransform = Mat4::translation(nodeOrigin);
    rootNode.localTransform = rootNode.baseLocalTransform;
    rootNode.globalTransform = rootNode.baseLocalTransform;
    rootNode.mesh = 0;
    rootNode.hasMatrix = true;
    rootNode.baseTranslation = {nodeOrigin.x(), nodeOrigin.y(), nodeOrigin.z()};
    rootNode.translation = rootNode.baseTranslation;
    model->nodes.push_back(rootNode);
    model->sceneRootNodes.push_back(0);
    GltfPrimitive primitive;
    primitive.vertices.resize(4);
    primitive.vertices[0].positionEcef = nodeOrigin + Vec3(0.0, 0.0, 0.0);
    primitive.vertices[1].positionEcef = nodeOrigin + Vec3(2.0, 0.0, 0.0);
    primitive.vertices[2].positionEcef = nodeOrigin + Vec3(0.0, 2.0, 0.0);
    primitive.vertices[3].positionEcef = nodeOrigin + Vec3(2.0, 2.0, 0.0);
    for (SurfaceVertex& vertex : primitive.vertices) {
        vertex.normalEcef = Vec3::unitZ();
    }
    primitive.vertices[0].uv = {0.0f, 0.0f};
    primitive.vertices[1].uv = {1.0f, 0.0f};
    primitive.vertices[2].uv = {0.0f, 1.0f};
    primitive.vertices[3].uv = {1.0f, 1.0f};
    primitive.vertexTexCoords[0] = {
        std::array<float, 2>{0.0f, 0.0f},
        std::array<float, 2>{1.0f, 0.0f},
        std::array<float, 2>{0.0f, 1.0f},
        std::array<float, 2>{1.0f, 1.0f}};
    primitive.indices = {0, 1, 2, 1, 3, 2};
    primitive.runtime.baseVertices = primitive.vertices;
    for (SurfaceVertex& vertex : primitive.runtime.baseVertices) {
        vertex.positionEcef = vertex.positionEcef - nodeOrigin;
    }
    primitive.runtime.nodeIndex = 0;
    primitive.runtime.hasNormals = true;
    model->primitives.push_back(std::move(primitive));
    model->rasterOverlayDetails.setGeographicRectangle(rectangle);
    return model;
}

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
    parent.content.renderContent.prepareGltfContent(
        makeQuadTerrainGltfModel(parent.bounds), Mat4::identity());
    parent.content.renderContent.setTerrainRenderContent(true);
    parent.content.renderContent.addGltfPrimitiveResource(
        GltfPrimitiveRenderResources{});
    parent.content.renderContent.markRenderContentReady();
    parent.markRenderContentDone();

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
    // Clip V is south-up (terrain texcoord0 keeps V=0 at the projected
    // south edge), so the north-east child quadrant starts at v=0.5.
    EXPECT_NEAR(entry.surfaceClipUv[0], 0.5f, 1e-6f);
    EXPECT_NEAR(entry.surfaceClipUv[1], 0.5f, 1e-6f);
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
    parent.content.renderContent.prepareGltfContent(
        makeQuadTerrainGltfModel(parent.bounds), Mat4::identity());
    parent.content.renderContent.setTerrainRenderContent(true);
    parent.content.renderContent.addGltfPrimitiveResource(
        GltfPrimitiveRenderResources{});
    parent.content.renderContent.markRenderContentReady();
    parent.markRenderContentDone();

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
    // Parent has glTF content but no primitive resources — not render-ready.
    parent.content.renderContent.setGltfContent(makeEmptyGltfModel());
    parent.content.loadState = TileLoadState::Done;
    parent.content.contentKind = TileContentKind::Render;
    // Child has glTF content at ContentLoaded so canAttemptRenderResourcePrep
    // returns true, allowing a direct render entry.
    child.content.renderContent.setGltfContent(makeEmptyGltfModel());
    child.content.renderContent.setTerrainRenderContent(true);
    child.content.loadState = TileLoadState::ContentLoaded;
    child.content.contentKind = TileContentKind::Render;

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

    // Parent is not drawable (no resources), so child gets a direct entry.
    // SynchronousPrep is dead — the entry reason is Direct.
    ASSERT_EQ(plan.renderEntries.size(), 1u);
    const TileRenderEntry& entry = plan.renderEntries.front();
    EXPECT_EQ(entry.selectedKey, childKey);
    EXPECT_EQ(entry.renderKey, childKey);
    EXPECT_EQ(entry.reason, TileRenderEntryReason::Direct);
    EXPECT_FALSE(entry.usesAncestorFallback);
    EXPECT_EQ(plan.renderEntryAncestorFallbackCount, 0);
    EXPECT_EQ(plan.renderEntrySynchronousPrepCount, 0);
    EXPECT_EQ(plan.renderEntryDeferredPrepCount, 0);
}

TEST(
    TileRenderPlanFinalizerTest,
    ContentProviderTerrainSurfaceResidueDoesNotBuildDirectEntry) {
    const TileKey rootKey{"test", 0, 0, 0};
    TilesetTile root(rootKey, Rectangle{});
    root.markRenderContentDone();
    ASSERT_FALSE(root.hasSurfaceDrawable());
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

    EXPECT_TRUE(plan.renderEntries.empty());
    EXPECT_EQ(plan.renderEntryAncestorFallbackCount, 0);
    EXPECT_EQ(plan.renderEntrySynchronousPrepCount, 0);
}

TEST(
    TileRenderPlanFinalizerTest,
    ContentProviderTerrainSurfaceResidueIsNotAncestorFallback) {
    const TileKey parentKey{"test", 0, 0, 0};
    const TileKey childKey{"test", 1, 1, 0};
    TilesetTile parent(parentKey, Rectangle{0.0, 0.0, 2.0, 2.0});
    TilesetTile child(childKey, Rectangle{1.0, 1.0, 2.0, 2.0}, &parent);
    parent.markRenderContentDone();
    ASSERT_FALSE(parent.hasSurfaceDrawable());
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

    EXPECT_TRUE(plan.renderEntries.empty());
    EXPECT_EQ(plan.renderEntryAncestorFallbackCount, 0);
    EXPECT_EQ(plan.renderEntrySynchronousPrepCount, 0);
}

TEST(TileRenderPlanFinalizerTest, CountsRootPrepOnceToAvoidBlankFrame) {
    const TileKey rootKey{"test", 0, 0, 0};
    TilesetTile root(rootKey, Rectangle{});
    makeGltfRenderReady(root);

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

    // With glTF content the root is directly renderable.
    ASSERT_EQ(plan.renderEntries.size(), 1u);
    const TileRenderEntry& entry = plan.renderEntries.front();
    EXPECT_EQ(entry.renderKey, rootKey);
    EXPECT_EQ(entry.reason, TileRenderEntryReason::Direct);
    EXPECT_EQ(plan.renderEntrySynchronousPrepCount, 0);
    EXPECT_EQ(plan.renderEntryDeferredPrepCount, 0);
}

TEST(TileRenderPlanFinalizerTest, DefersFallbackPrepDuringInteraction) {
    const TileKey parentKey{"test", 0, 0, 0};
    const TileKey childKey{"test", 1, 0, 0};
    TilesetTile parent(parentKey, Rectangle{0.0, 0.0, 2.0, 2.0});
    TilesetTile child(childKey, Rectangle{0.0, 1.0, 1.0, 2.0}, &parent);
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
        [&parent](const TilesetTile& tile) {
            return tile.key == parent.key;
        });

    // With glTF content on parent, ancestor fallback works.
    // Deferred prep is dead — allowSynchronousMeshPrep is always true.
    ASSERT_EQ(plan.renderEntries.size(), 1u);
    const TileRenderEntry& entry = plan.renderEntries.front();
    EXPECT_EQ(entry.selectedKey, childKey);
    EXPECT_EQ(entry.renderKey, parentKey);
    EXPECT_EQ(entry.reason, TileRenderEntryReason::AncestorFallback);
    EXPECT_TRUE(entry.usesAncestorFallback);
    EXPECT_TRUE(entry.allowSynchronousMeshPrep);
    EXPECT_EQ(plan.renderEntryAncestorFallbackCount, 1);
    EXPECT_EQ(plan.renderEntrySynchronousPrepCount, 0);
    EXPECT_EQ(plan.renderEntryDeferredPrepCount, 0);
}

TEST(
    TileRenderPlanFinalizerTest,
    ReadsSelectionFrameFadeWhenLodTransitionEnabled) {
    const TileKey rootKey{"test", 0, 0, 0};
    TilesetTile root(rootKey, Rectangle{});
    makeGltfRenderReady(root);
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

    // With glTF content the root is directly renderable.
    ASSERT_EQ(plan.renderEntries.size(), 1u);
    EXPECT_EQ(
        plan.renderEntries.front().reason,
        TileRenderEntryReason::Direct);
    EXPECT_NEAR(plan.renderEntries.front().opacity, 0.25f, 1e-6f);
}

TEST(TileRenderPlanFinalizerTest, FadingTilesBecomeFadePassEntries) {
    const TileKey fadingKey{"test", 0, 0, 0};
    TilesetTile fading(fadingKey, Rectangle{});
    fading.content.renderContent.prepareGltfContent(
        makeQuadTerrainGltfModel(fading.bounds), Mat4::identity());
    fading.content.renderContent.setTerrainRenderContent(true);
    fading.content.renderContent.addGltfPrimitiveResource(
        GltfPrimitiveRenderResources{});
    fading.content.renderContent.markRenderContentReady();
    fading.markRenderContentDone();

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
