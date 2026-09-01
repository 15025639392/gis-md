#include <gtest/gtest.h>

#include "earth_engine/content/GltfModel.h"
#include "earth_engine/core/geodesy/Ellipsoid.h"
#include "earth_engine/core/geodesy/Projection.h"
#include "earth_engine/core/geodesy/WebMercatorProjection.h"
#include "earth_engine/core/math/Mat4.h"
#include "earth_engine/layers/ActivatedRasterOverlay.h"
#include "earth_engine/layers/RasterOverlay.h"
#include "earth_engine/providers/DebugImageryProvider.h"
#include "earth_engine/providers/RasterOverlayTile.h"
#include "earth_engine/providers/RasterOverlayTileProvider.h"
#include "earth_engine/providers/TerrainProvider.h"
#include "earth_engine/renderer/RenderDevice.h"
#include "earth_engine/tiling/DirectRasterMapping.h"
#include "earth_engine/core/resources/FrameResourceBudget.h"
#include "earth_engine/tiling/TileCacheKey.h"
#include "earth_engine/tiling/TileRasterOverlayPrefetcher.h"
#include "earth_engine/tiling/TileRasterOverlayReadinessPolicy.h"
#include "earth_engine/tiling/TileRenderPlanFinalizer.h"
#include "earth_engine/tiling/RasterOverlayRuntime.h"
#include "earth_engine/tiling/TileScheme.h"
#include "../../helpers/RasterOverlayTestFrame.h"

#include <array>
#include <cmath>
#include <memory>
#include <type_traits>
#include <unordered_map>

using namespace earth_engine;

namespace {

static_assert(!std::is_copy_constructible_v<TilePlan>);
static_assert(!std::is_copy_assignable_v<TilePlan>);

RasterOverlayFrameContext frozenRasterFrame(
    const std::vector<ActivatedRasterOverlay*>& overlays) {
    RasterOverlayRuntime runtime(overlays);
    runtime.beginFrame(1, nullptr);
    return runtime.frameContext();
}

bool terrainSurfaceImageryDrawableReady(
    const TilesetTile& tile,
    const std::vector<ActivatedRasterOverlay*>& overlays) {
    const RasterOverlayFrameContext frame = frozenRasterFrame(overlays);
    return TileRasterOverlayReadinessPolicy::
        terrainSurfaceImageryDrawableReady(tile, frame);
}

BaseImageryBlockReason baseImageryBlockReason(
    const TilesetTile& tile,
    const std::vector<ActivatedRasterOverlay*>& overlays) {
    const RasterOverlayFrameContext frame = frozenRasterFrame(overlays);
    return TileRasterOverlayReadinessPolicy::baseImageryBlockReason(
        tile, frame);
}

BaseImageryNoTextureProbe probeNoReadyTexture(
    const TilesetTile& tile,
    const std::vector<ActivatedRasterOverlay*>& overlays) {
    const RasterOverlayFrameContext frame = frozenRasterFrame(overlays);
    return TileRasterOverlayReadinessPolicy::probeNoReadyTexture(tile, frame);
}

TileRenderPlanFinalizeOptions testFinalizeOptions(
    bool interactionActive,
    int activeInteractionRenderPrepBudget,
    int recoveryRenderPrepBudget,
    double maximumScreenSpaceError = 0.0,
    int activeInteractionFirstBuildBudget = 4,
    int recoveryFirstBuildBudget = 8) {
    return TileRenderPlanFinalizeOptions{
        interactionActive,
        activeInteractionRenderPrepBudget,
        recoveryRenderPrepBudget,
        maximumScreenSpaceError,
        activeInteractionFirstBuildBudget,
        recoveryFirstBuildBudget,
        earth_engine::testing::emptyRasterOverlayFrame()};
}

template <typename EnsureTileFn,
          typename CacheKeyFn,
          typename IsFallbackRenderableFn>
void refreshRenderEntries(
    TilePlan& plan,
    TileRenderPlanFinalizeOptions options,
    EnsureTileFn&& ensureTile,
    CacheKeyFn&& cacheKey,
    IsFallbackRenderableFn&& isFallbackRenderable) {
    static const std::vector<ActivatedRasterOverlay*> kNoOverlays;
    RasterOverlayRuntime runtime(kNoOverlays);
    runtime.beginFrame(plan.frameId, nullptr);
    const TileRenderPlanFinalizeOptions frameOptions{
        options.interactionActive,
        options.activeInteractionRenderPrepBudget,
        options.recoveryRenderPrepBudget,
        options.maximumScreenSpaceError,
        options.activeInteractionFirstBuildBudget,
        options.recoveryFirstBuildBudget,
        runtime.frameContext()};
    TileRenderPlanFinalizer::refreshRenderEntries(
        plan,
        frameOptions,
        std::forward<EnsureTileFn>(ensureTile),
        std::forward<CacheKeyFn>(cacheKey),
        std::forward<IsFallbackRenderableFn>(isFallbackRenderable));
}

template <typename EnsureTileFn,
          typename CacheKeyFn,
          typename IsFallbackRenderableFn>
void refreshRenderEntries(
    TilePlan& plan,
    TileRenderPlanFinalizeOptions options,
    const std::vector<ActivatedRasterOverlay*>& overlays,
    EnsureTileFn&& ensureTile,
    CacheKeyFn&& cacheKey,
    IsFallbackRenderableFn&& isFallbackRenderable) {
    RasterOverlayRuntime runtime(overlays);
    runtime.beginFrame(plan.frameId, nullptr);
    const TileRenderPlanFinalizeOptions frameOptions{
        options.interactionActive,
        options.activeInteractionRenderPrepBudget,
        options.recoveryRenderPrepBudget,
        options.maximumScreenSpaceError,
        options.activeInteractionFirstBuildBudget,
        options.recoveryFirstBuildBudget,
        runtime.frameContext()};
    TileRenderPlanFinalizer::refreshRenderEntries(
        plan,
        frameOptions,
        std::forward<EnsureTileFn>(ensureTile),
        std::forward<CacheKeyFn>(cacheKey),
        std::forward<IsFallbackRenderableFn>(isFallbackRenderable));
}

class DummyBuffer final : public Buffer {
public:
    explicit DummyBuffer(size_t byteSize) : byteSize_(byteSize) {}
    size_t size() const override { return byteSize_; }

private:
    size_t byteSize_ = 0;
};

class DummyTexture final : public Texture {
public:
    DummyTexture(int width, int height) : width_(width), height_(height) {}
    int width() const override { return width_; }
    int height() const override { return height_; }
    size_t sizeBytes() const override {
        return static_cast<size_t>(width_) *
               static_cast<size_t>(height_) * 4u;
    }

private:
    int width_ = 0;
    int height_ = 0;
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
    // Native uv and overlay texcoord set 0 are both NW-based (v=0 at the
    // north edge = position y=2); QuantizedMeshParser decodes native uv
    // into the same convention.
    primitive.vertices[0].uv = {0.0f, 1.0f};
    primitive.vertices[1].uv = {1.0f, 1.0f};
    primitive.vertices[2].uv = {0.0f, 0.0f};
    primitive.vertices[3].uv = {1.0f, 0.0f};
    primitive.vertexTexCoords[0] = {
        std::array<float, 2>{0.0f, 1.0f},
        std::array<float, 2>{1.0f, 1.0f},
        std::array<float, 2>{0.0f, 0.0f},
        std::array<float, 2>{1.0f, 0.0f}};
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

void makeEllipsoidFallbackRenderReady(TilesetTile& tile) {
    makeGltfRenderReady(tile);
    tile.content.renderContent.setSurfaceSource(
        SurfaceDrawableSource::EllipsoidFallback);
}

void makeFillRenderReady(
    TilesetTile& tile,
    RasterOverlayProjection projection) {
    tile.content.renderContent.setFillContent(
        makeQuadTerrainGltfModel(tile.bounds));
    GltfPrimitiveRenderResources resources;
    resources.useTerrainVertexFormat = true;
    resources.vertexBuffer = std::make_unique<DummyBuffer>(64);
    resources.indexBuffer = std::make_unique<DummyBuffer>(24);
    resources.vertexCount = 4;
    resources.indexCount = 6;
    tile.content.renderContent.beginFillGpuResourceBuild(0, 1);
    tile.content.renderContent.addFillPrimitiveResource(
        std::move(resources));
    const std::optional<TileFillGeometrySignature> signature =
        TileFillGeometrySignature::tryCreate(
            tile.bounds,
            projection,
            4);
    ASSERT_TRUE(signature.has_value());
    tile.content.renderContent.commitFillResourcesReady(*signature);
    tile.content.renderContent.clearFillCpuModelAfterUpload();
    ASSERT_TRUE(tile.content.renderContent.isFillReady());
    ASSERT_TRUE(tile.content.renderContent.drawsFill());
}

bool isDrawableRenderContent(const TilesetTile& tile) {
    return tile.content.renderContent.hasDrawableResources();
}

std::unique_ptr<RasterOverlay> makeBlockingBaseOverlay() {
    RasterOverlay::Options options;
    options.role = RasterOverlayRole::BaseImagery;
    options.blocksCompleteRenderable = true;
    return std::make_unique<RasterOverlay>(
        std::make_unique<DebugImageryProvider>(),
        TileScheme::createGeographicTMS(),
        options);
}

void makeDrawableBaseRaster(TilesetTile& tile,
                            ActivatedRasterOverlay& activeOverlay) {
    RasterOverlayTileProvider* provider =
        activeOverlay.ensureTileProvider(nullptr);
    ASSERT_NE(nullptr, provider);

    std::vector<RasterOverlayProjection> missingProjections;
    DirectRasterMapping& mapped =
        tile.rasterOverlayState.ensureMapping(0);
    mapped.update(
        tile.key,
        tile.content.renderContent.rasterOverlayDetails(),
        256.0,
        256.0,
        *provider,
        nullptr,
        missingProjections,
        tile.parent,
        0);
    RasterOverlayTile* loadingTile = mapped.getLoadingTile();
    ASSERT_NE(nullptr, loadingTile);
    loadingTile->setTexture(std::make_unique<DummyTexture>(4, 4));
    mapped.update(
        tile.key,
        tile.content.renderContent.rasterOverlayDetails(),
        256.0,
        256.0,
        *provider,
        nullptr,
        missingProjections,
        tile.parent,
        0);
    ASSERT_TRUE(tile.rasterOverlayState.hasDrawableReadyMapping(0));
}

} // namespace

TEST(
    TileRenderPlanFinalizerTest,
    UsesSelectionLiveHandleWithoutResolvingKeyAgain) {
    const TileKey rootKey{"test", 0, 0, 0};
    TilesetTile root(rootKey, Rectangle{});
    makeGltfRenderReady(root);

    TilePlan plan;
    plan.visibleTiles.push_back(rootKey);
    plan.tilesToRenderThisFrame.push_back(&root);
    int ensureCalls = 0;

    refreshRenderEntries(
        plan,
        testFinalizeOptions(false, true, 0, 1),
        [&ensureCalls](const TileKey&) -> TilesetTile* {
            ++ensureCalls;
            return nullptr;
        },
        [](const TileKey& key) {
            return TileCacheKey::forTile(key);
        },
        [](const TilesetTile& tile) {
            return isDrawableRenderContent(tile);
        });

    EXPECT_EQ(ensureCalls, 0);
    ASSERT_EQ(plan.tilesToRenderThisFrame.size(), 1u);
    EXPECT_EQ(plan.tilesToRenderThisFrame.front(), &root);
    ASSERT_EQ(plan.renderEntries.size(), 1u);
    EXPECT_EQ(plan.renderEntries.front().selectedTile, &root);
    EXPECT_EQ(plan.renderEntries.front().renderTile, &root);
}

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
    // Height-remap now requires a real retained DEM to sample; without one a
    // Geographic tile no longer reports "supports remap" (no height data to
    // read). Install a minimal heightmap so the Geographic ancestor-fallback
    // remap path is exercised as intended.
    {
        auto hm = std::make_unique<DecodedHeightmap>();
        hm->tileSize = 2;
        hm->quantizedHeights.assign(4u, 64u);  // 码 64 → quantBase+8m flat
        hm->quantBase = 0;
        hm->minHeight = 8.0f;
        hm->maxHeight = 8.0f;
        parent.content.renderContent.setRetainedHeightmap(std::move(hm));
    }
    parent.content.renderContent.markRenderContentReady();
    parent.markRenderContentDone();

    std::unordered_map<std::string, TilesetTile*> tiles{
        {TileCacheKey::forTile(parentKey), &parent},
        {TileCacheKey::forTile(childKey), &child}};

    TilePlan plan;
    plan.visibleTiles.push_back(childKey);
    refreshRenderEntries(
        plan,
        testFinalizeOptions(false, true, 0, 1),
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
    ASSERT_EQ(plan.tilesToRenderThisFrame.size(), 1u);
    EXPECT_EQ(plan.tilesToRenderThisFrame.front(), &child);
    const TileRenderEntry& entry = plan.renderEntries.front();
    EXPECT_EQ(entry.selectedKey, childKey);
    EXPECT_EQ(entry.renderKey, parentKey);
    EXPECT_EQ(entry.selectedTile, &child);
    EXPECT_EQ(entry.renderTile, &parent);
    EXPECT_EQ(entry.reason, TileRenderEntryReason::AncestorFallback);
    EXPECT_TRUE(entry.usesAncestorFallback);
    EXPECT_TRUE(entry.surfaceClipEnabled);
    EXPECT_TRUE(TileSurfaceClip::supportsTerrainHeightRemap(parent));
    EXPECT_EQ(plan.renderEntryAncestorFallbackCount, 1);
    EXPECT_EQ(plan.renderEntrySynchronousPrepCount, 0);
    EXPECT_EQ(plan.renderEntryDeferredPrepCount, 0);
    // Clip V is NW-based (terrain texcoord0 keeps V=0 at the projected
    // north edge), so the north-east child quadrant starts at v=0.
    // 子象限 (0.5,0,0.5,0.5) 按封缝带外扩 3% 跨度并 clamp(见
    // TileSurfaceClip::kClipSeamSealMarginFraction)。
    EXPECT_NEAR(entry.surfaceClipUv[0], 0.485f, 1e-6f);
    EXPECT_NEAR(entry.surfaceClipUv[1], 0.0f, 1e-6f);
    EXPECT_NEAR(entry.surfaceClipUv[2], 0.515f, 1e-6f);
    EXPECT_NEAR(entry.surfaceClipUv[3], 0.515f, 1e-6f);
}

// never-drop(cesium 语义):几何可画但 blocking base 影像未就绪、且无可回落祖先
// 时,**不再 drop 成透底洞**,而是发一条 base 色 entry(命令端出 count=0 纯色面,
// 细化继续追真影像)。原为 SkipsTerrainDirectEntryUntilBlockingBaseImageryIsDrawable,
// 锁"升级前"的 skip;现锁升级后的兜底。
TEST(
    TileRenderPlanFinalizerTest,
    EmitsBaseColorEntryWhenBlockingBaseImageryMissing) {
    const TileKey rootKey{"test", 0, 0, 0};
    TilesetTile root(rootKey, Rectangle{0.0, 0.0, 2.0, 2.0});
    root.content.renderContent.prepareGltfContent(
        makeQuadTerrainGltfModel(root.bounds), Mat4::identity());
    root.content.renderContent.setTerrainRenderContent(true);
    root.content.renderContent.addGltfPrimitiveResource(
        GltfPrimitiveRenderResources{});
    root.content.renderContent.markRenderContentReady();
    root.markRenderContentDone();

    auto baseOverlay = makeBlockingBaseOverlay();
    ActivatedRasterOverlay activeBase(*baseOverlay);
    std::vector<ActivatedRasterOverlay*> overlays{&activeBase};

    TilePlan plan;
    plan.visibleTiles.push_back(rootKey);
    refreshRenderEntries(
        plan,
        testFinalizeOptions(false, true, 0, 1),
        overlays,
        [&root](const TileKey& key) -> TilesetTile* {
            return key == root.key ? &root : nullptr;
        },
        [](const TileKey& key) {
            return TileCacheKey::forTile(key);
        },
        [&overlays](const TilesetTile& tile) {
            return isDrawableRenderContent(tile) &&
                   terrainSurfaceImageryDrawableReady(tile, overlays);
        });

    // never-drop:发一条自身的 base 色 entry,不 drop、不用祖先(本就没有)。
    ASSERT_EQ(plan.renderEntries.size(), 1u);
    EXPECT_EQ(plan.renderEntries[0].selectedKey, rootKey);
    EXPECT_EQ(plan.renderEntries[0].reason, TileRenderEntryReason::Direct);
    EXPECT_EQ(plan.renderEntryBaseColorFallbackCount, 1);
    EXPECT_EQ(plan.renderEntryDropNotBuildableCount, 0);
    EXPECT_EQ(plan.renderEntryAncestorFallbackCount, 0);
}

TEST(
    TileRenderPlanFinalizerTest,
    UsesTexturedAncestorWhenSelectedTerrainBaseImageryIsMissing) {
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
    child.content.renderContent.prepareGltfContent(
        makeQuadTerrainGltfModel(child.bounds), Mat4::identity());
    child.content.renderContent.setTerrainRenderContent(true);
    child.content.renderContent.addGltfPrimitiveResource(
        GltfPrimitiveRenderResources{});
    child.content.renderContent.markRenderContentReady();
    child.markRenderContentDone();

    auto baseOverlay = makeBlockingBaseOverlay();
    ActivatedRasterOverlay activeBase(*baseOverlay);
    std::vector<ActivatedRasterOverlay*> overlays{&activeBase};
    makeDrawableBaseRaster(parent, activeBase);

    std::unordered_map<std::string, TilesetTile*> tiles{
        {TileCacheKey::forTile(parentKey), &parent},
        {TileCacheKey::forTile(childKey), &child}};

    TilePlan plan;
    plan.visibleTiles.push_back(childKey);
    refreshRenderEntries(
        plan,
        testFinalizeOptions(false, true, 0, 1),
        overlays,
        [&tiles](const TileKey& key) {
            return findTile(tiles, key);
        },
        [](const TileKey& key) {
            return TileCacheKey::forTile(key);
        },
        [&overlays](const TilesetTile& tile) {
            return isDrawableRenderContent(tile) &&
                   terrainSurfaceImageryDrawableReady(tile, overlays);
        });

    ASSERT_EQ(plan.renderEntries.size(), 1u);
    const TileRenderEntry& entry = plan.renderEntries.front();
    EXPECT_EQ(entry.selectedKey, childKey);
    EXPECT_EQ(entry.renderKey, parentKey);
    EXPECT_EQ(entry.reason, TileRenderEntryReason::AncestorFallback);
    EXPECT_TRUE(entry.usesAncestorFallback);
    EXPECT_TRUE(entry.surfaceClipEnabled);
    EXPECT_EQ(plan.renderEntryAncestorFallbackCount, 1);
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
    refreshRenderEntries(
        plan,
        testFinalizeOptions(false, false, 0, 1),
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
    refreshRenderEntries(
        plan,
        testFinalizeOptions(false, true, 0, 1),
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
    refreshRenderEntries(
        plan,
        testFinalizeOptions(false, true, 0, 1),
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
    UsesReadyAncestorWhileSelectedEllipsoidFallbackIsReady) {
    const TileKey parentKey{"test", 0, 0, 0};
    const TileKey childKey{"test", 1, 1, 0};
    TilesetTile parent(parentKey, Rectangle{0.0, 0.0, 2.0, 2.0});
    TilesetTile child(childKey, Rectangle{1.0, 1.0, 2.0, 2.0}, &parent);
    makeGltfRenderReady(parent);
    makeEllipsoidFallbackRenderReady(child);
    ASSERT_TRUE(child.content.renderContent.hasDrawableResources());
    ASSERT_TRUE(
        child.content.renderContent.drawsTransientFallbackSurface());

    std::unordered_map<std::string, TilesetTile*> tiles{
        {TileCacheKey::forTile(parentKey), &parent},
        {TileCacheKey::forTile(childKey), &child}};

    TilePlan plan;
    plan.visibleTiles.push_back(childKey);
    refreshRenderEntries(
        plan,
        testFinalizeOptions(false, true, 0, 1),
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
    EXPECT_EQ(plan.renderEntryAncestorFallbackCount, 1);
}

TEST(
    TileRenderPlanFinalizerTest,
    UsesReadyAncestorWhileSelectedFillProxyIsReady) {
    const TileKey parentKey{"test", 0, 0, 0};
    const TileKey childKey{"test", 1, 1, 0};
    TilesetTile parent(parentKey, Rectangle{0.0, 0.0, 2.0, 2.0});
    TilesetTile child(childKey, Rectangle{1.0, 1.0, 2.0, 2.0}, &parent);
    makeGltfRenderReady(parent);
    makeFillRenderReady(child, RasterOverlayProjection::Geographic);
    ASSERT_TRUE(child.content.renderContent.hasDrawableResources());
    ASSERT_TRUE(
        child.content.renderContent.drawsTransientFallbackSurface());

    std::unordered_map<std::string, TilesetTile*> tiles{
        {TileCacheKey::forTile(parentKey), &parent},
        {TileCacheKey::forTile(childKey), &child}};

    TilePlan plan;
    plan.visibleTiles.push_back(childKey);
    refreshRenderEntries(
        plan,
        testFinalizeOptions(false, true, 0, 1),
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
    EXPECT_EQ(plan.renderEntryAncestorFallbackCount, 1);
}

TEST(
    TileRenderPlanFinalizerTest,
    UsesReadyGeographicFillAncestorFallbackWithSurfaceClip) {
    const TileKey parentKey{"test", 0, 0, 0};
    const TileKey childKey{"test", 1, 1, 0};
    TilesetTile parent(
        parentKey,
        Rectangle::fromDegrees(0.0, 0.0, 20.0, 20.0));
    TilesetTile child(
        childKey,
        Rectangle::fromDegrees(10.0, 10.0, 20.0, 20.0),
        &parent);
    makeFillRenderReady(parent, RasterOverlayProjection::Geographic);

    std::unordered_map<std::string, TilesetTile*> tiles{
        {TileCacheKey::forTile(parentKey), &parent},
        {TileCacheKey::forTile(childKey), &child}};
    TilePlan plan;
    plan.visibleTiles.push_back(childKey);

    refreshRenderEntries(
        plan,
        testFinalizeOptions(false, true, 0, 1),
        [&tiles](const TileKey& key) {
            return findTile(tiles, key);
        },
        [](const TileKey& key) {
            return TileCacheKey::forTile(key);
        },
        [](const TilesetTile& tile) {
            return isDrawableRenderContent(tile);
        });

    ASSERT_EQ(1u, plan.renderEntries.size());
    const TileRenderEntry& entry = plan.renderEntries.front();
    EXPECT_EQ(&parent, entry.renderTile);
    EXPECT_TRUE(entry.usesAncestorFallback);
    EXPECT_TRUE(entry.surfaceClipEnabled);
    // 子象限外扩封缝带(见 TileSurfaceClip::kClipSeamSealMarginFraction)。
    EXPECT_NEAR(0.485f, entry.surfaceClipUv[0], 1e-6f);
    EXPECT_NEAR(0.0f, entry.surfaceClipUv[1], 1e-6f);
    EXPECT_NEAR(0.515f, entry.surfaceClipUv[2], 1e-6f);
    EXPECT_NEAR(0.515f, entry.surfaceClipUv[3], 1e-6f);
}

TEST(
    TileRenderPlanFinalizerTest,
    UsesReadyWebMercatorFillAncestorFallbackWithProjectedClip) {
    const TileKey parentKey{"test", 0, 0, 0};
    const TileKey childKey{"test", 1, 1, 0};
    TilesetTile parent(
        parentKey,
        Rectangle::fromDegrees(0.0, 0.0, 20.0, 20.0));
    TilesetTile child(
        childKey,
        Rectangle::fromDegrees(10.0, 10.0, 20.0, 20.0),
        &parent);
    makeFillRenderReady(parent, RasterOverlayProjection::WebMercator);

    std::unordered_map<std::string, TilesetTile*> tiles{
        {TileCacheKey::forTile(parentKey), &parent},
        {TileCacheKey::forTile(childKey), &child}};
    TilePlan plan;
    plan.visibleTiles.push_back(childKey);

    refreshRenderEntries(
        plan,
        testFinalizeOptions(false, true, 0, 1),
        [&tiles](const TileKey& key) {
            return findTile(tiles, key);
        },
        [](const TileKey& key) {
            return TileCacheKey::forTile(key);
        },
        [](const TilesetTile& tile) {
            return isDrawableRenderContent(tile);
        });

    const WebMercatorProjection projection(Ellipsoid::WGS84());
    const Rectangle parentProjected =
        projectRectangleSimple(projection, parent.bounds);
    const Rectangle childProjected =
        projectRectangleSimple(projection, child.bounds);
    const float expectedVScale = static_cast<float>(
        (parentProjected.north() - childProjected.south()) /
        parentProjected.computeHeight());

    ASSERT_EQ(1u, plan.renderEntries.size());
    const TileRenderEntry& entry = plan.renderEntries.front();
    EXPECT_EQ(&parent, entry.renderTile);
    EXPECT_TRUE(entry.usesAncestorFallback);
    EXPECT_TRUE(entry.surfaceClipEnabled);
    EXPECT_FALSE(TileSurfaceClip::supportsTerrainHeightRemap(parent));
    // 子象限外扩封缝带:v 起点 0 处 clamp,高度 = 1.03×投影跨度(见
    // TileSurfaceClip::kClipSeamSealMarginFraction)。
    EXPECT_NEAR(0.485f, entry.surfaceClipUv[0], 1e-6f);
    EXPECT_NEAR(0.0f, entry.surfaceClipUv[1], 1e-6f);
    EXPECT_NEAR(0.515f, entry.surfaceClipUv[2], 1e-6f);
    EXPECT_NEAR(expectedVScale * 1.03f, entry.surfaceClipUv[3], 1e-5f);
    EXPECT_GT(std::abs(entry.surfaceClipUv[3] - 0.5f), 1e-3f);
}

TEST(
    TileRenderPlanFinalizerTest,
    UnreadyStaticGltfDoesNotCreateDirectRenderEntry) {
    const TileKey parentKey{"test", 0, 0, 0};
    const TileKey childKey{"test", 1, 1, 0};
    TilesetTile parent(parentKey, Rectangle{0.0, 0.0, 2.0, 2.0});
    TilesetTile child(childKey, Rectangle{1.0, 1.0, 2.0, 2.0}, &parent);
    // Parent has glTF content but no primitive resources — not render-ready.
    parent.content.renderContent.setGltfContent(makeEmptyGltfModel());
    parent.content.loadState = TileLoadState::Done;
    parent.content.contentKind = TileContentKind::Render;
    // Child has committed static glTF content but no GPU resources yet.
    child.content.renderContent.setGltfContent(makeEmptyGltfModel());
    child.content.renderContent.setTerrainRenderContent(true);
    child.content.loadState = TileLoadState::ContentLoaded;
    child.content.contentKind = TileContentKind::Render;

    std::unordered_map<std::string, TilesetTile*> tiles{
        {TileCacheKey::forTile(parentKey), &parent},
        {TileCacheKey::forTile(childKey), &child}};

    TilePlan plan;
    plan.visibleTiles.push_back(childKey);
    refreshRenderEntries(
        plan,
        testFinalizeOptions(false, true, 0, 1),
        [&tiles](const TileKey& key) {
            return findTile(tiles, key);
        },
        [](const TileKey& key) {
            return TileCacheKey::forTile(key);
        },
        [](const TilesetTile& tile) {
            return isDrawableRenderContent(tile);
        });

    // Neither tile is drawable. Resource creation belongs to update/upload,
    // so draw planning must wait instead of creating a direct prep entry.
    EXPECT_TRUE(plan.renderEntries.empty());
    ASSERT_EQ(1u, plan.tilesToRenderThisFrame.size());
    EXPECT_EQ(&child, plan.tilesToRenderThisFrame.front());
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
    refreshRenderEntries(
        plan,
        testFinalizeOptions(false, true, 0, 1),
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
    refreshRenderEntries(
        plan,
        testFinalizeOptions(false, true, 0, 1),
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
    refreshRenderEntries(
        plan,
        testFinalizeOptions(false, true, 0, 1),
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
    refreshRenderEntries(
        plan,
        testFinalizeOptions(false, true, 0, 1),
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

// 破洞诊断第三轮:真机实测的残余 drop 形态 —— mapping 建过、影像也已 Loaded,
// 却因为几何瓦片停在 Failed 态、再没有任何路径推进 Direct raster attachment,Loading→Ready
// 的提升永远不发生。这个测试把那一帧的状态原样搭出来,钉死探针的读数含义:
// load=Loaded / ready=空 / 祖先链一个 mapping 都没有。
TEST(
    TileRenderPlanFinalizerTest,
    ProbesUnpromotedLoadedImageryAsNoReadyTexture) {
    const TileKey rootKey{"test", 0, 0, 0};
    TilesetTile root(rootKey, Rectangle{0.0, 0.0, 2.0, 2.0});
    root.content.renderContent.prepareGltfContent(
        makeQuadTerrainGltfModel(root.bounds), Mat4::identity());
    root.content.renderContent.setTerrainRenderContent(true);
    root.content.renderContent.addGltfPrimitiveResource(
        GltfPrimitiveRenderResources{});
    root.content.renderContent.markRenderContentReady();
    root.markRenderContentDone();

    auto baseOverlay = makeBlockingBaseOverlay();
    ActivatedRasterOverlay activeBase(*baseOverlay);
    std::vector<ActivatedRasterOverlay*> overlays{&activeBase};

    RasterOverlayTileProvider* provider =
        activeBase.ensureTileProvider(nullptr);
    ASSERT_NE(nullptr, provider);
    std::vector<RasterOverlayProjection> missingProjections;
    DirectRasterMapping& mapped =
        root.rasterOverlayState.ensureMapping(0);
    mapped.update(
        rootKey,
        root.content.renderContent.rasterOverlayDetails(),
        256.0,
        256.0,
        *provider,
        nullptr,
        missingProjections,
        root.parent,
        0);
    RasterOverlayTile* loadingTile = mapped.getLoadingTile();
    ASSERT_NE(nullptr, loadingTile);
    // 影像到手(Loaded),但故意不再调 update() —— 真机上正是这一步没人替它做。
    loadingTile->setTexture(std::make_unique<DummyTexture>(4, 4));

    EXPECT_EQ(
        baseImageryBlockReason(
            root,
            overlays),
        BaseImageryBlockReason::NoReadyTexture);

    const BaseImageryNoTextureProbe probe =
        probeNoReadyTexture(root, overlays);
    EXPECT_TRUE(probe.valid);
    EXPECT_EQ(probe.zoom, 0);
    EXPECT_EQ(
        probe.loadingState,
        static_cast<int>(RasterOverlayTile::LoadState::Loaded));
    EXPECT_EQ(probe.readyState, -1);
    EXPECT_FALSE(probe.readyHasTexture);
    EXPECT_EQ(probe.ancestorDepth, 0);
    EXPECT_EQ(probe.ancestorsWithMapping, 0);
    EXPECT_EQ(probe.ancestorsWithTexture, 0);

    TilePlan plan;
    plan.visibleTiles.push_back(rootKey);
    refreshRenderEntries(
        plan,
        testFinalizeOptions(false, true, 0, 1),
        overlays,
        [&root](const TileKey& key) -> TilesetTile* {
            return key == root.key ? &root : nullptr;
        },
        [](const TileKey& key) {
            return TileCacheKey::forTile(key);
        },
        [&overlays](const TilesetTile& tile) {
            return isDrawableRenderContent(tile) &&
                   terrainSurfaceImageryDrawableReady(tile, overlays);
        });

    // 探针(上面)仍读 NoReadyTexture —— 那是就绪判据,never-drop 不改它。
    // 但 finalizer 不再据此 drop:几何可画 → 发 base 色 entry,drop 桶归零,
    // base 色兜底计数 +1。诊断需要"为什么是灰"时仍可按需调 probeNoReadyTexture。
    ASSERT_EQ(plan.renderEntries.size(), 1u);
    EXPECT_EQ(plan.renderEntries[0].selectedKey, rootKey);
    EXPECT_EQ(plan.renderEntryDropNotBuildableCount, 0);
    EXPECT_EQ(plan.renderEntryDropNoReadyTextureCount, 0);
    EXPECT_EQ(plan.renderEntryBaseColorFallbackCount, 1);
}

// 上一个测试钉住的病:影像 Loaded 了却没人提升。修法 = 让节流泵既发也收。
// 这里验证泵跑一次之后瓦片重新可画,并且 finalizer 不再丢弃它。
TEST(
    TileRenderPlanFinalizerTest,
    ThrottledPumpPromotesLoadedImageryWithoutFullUpdate) {
    const TileKey rootKey{"test", 0, 0, 0};
    TilesetTile root(rootKey, Rectangle{0.0, 0.0, 2.0, 2.0});
    root.content.renderContent.prepareGltfContent(
        makeQuadTerrainGltfModel(root.bounds), Mat4::identity());
    root.content.renderContent.setTerrainRenderContent(true);
    root.content.renderContent.addGltfPrimitiveResource(
        GltfPrimitiveRenderResources{});
    root.content.renderContent.markRenderContentReady();
    root.markRenderContentDone();

    auto baseOverlay = makeBlockingBaseOverlay();
    ActivatedRasterOverlay activeBase(*baseOverlay);
    std::vector<ActivatedRasterOverlay*> overlays{&activeBase};

    RasterOverlayTileProvider* provider =
        activeBase.ensureTileProvider(nullptr);
    ASSERT_NE(nullptr, provider);
    std::vector<RasterOverlayProjection> missingProjections;
    DirectRasterMapping& mapped =
        root.rasterOverlayState.ensureMapping(0);
    mapped.update(
        rootKey,
        root.content.renderContent.rasterOverlayDetails(),
        256.0,
        256.0,
        *provider,
        nullptr,
        missingProjections,
        root.parent,
        0);
    RasterOverlayTile* loadingTile = mapped.getLoadingTile();
    ASSERT_NE(nullptr, loadingTile);
    loadingTile->setTexture(std::make_unique<DummyTexture>(4, 4));
    ASSERT_FALSE(
        terrainSurfaceImageryDrawableReady(
            root,
            overlays));

    // 泵一次 —— 这是这批瓦片唯一还会被调用到的 Direct raster 推进路径。
    FrameResourceBudget budget;
    TileRasterOverlayPrefetcher::advanceThrottledLoads(
        root,
        overlays,
        TileRasterOverlayReadinessPolicy::processingOrder(overlays),
        nullptr,
        budget);

    EXPECT_EQ(mapped.getLoadingTile(), nullptr);
    ASSERT_NE(mapped.getReadyTile(), nullptr);
    EXPECT_NE(mapped.getReadyTile()->getTexture(), nullptr);
    EXPECT_TRUE(
        terrainSurfaceImageryDrawableReady(
            root,
            overlays));

    TilePlan plan;
    plan.visibleTiles.push_back(rootKey);
    refreshRenderEntries(
        plan,
        testFinalizeOptions(false, true, 0, 1),
        overlays,
        [&root](const TileKey& key) -> TilesetTile* {
            return key == root.key ? &root : nullptr;
        },
        [](const TileKey& key) {
            return TileCacheKey::forTile(key);
        },
        [&overlays](const TilesetTile& tile) {
            return isDrawableRenderContent(tile) &&
                   terrainSurfaceImageryDrawableReady(tile, overlays);
        });

    EXPECT_EQ(plan.renderEntries.size(), 1u);
    EXPECT_EQ(plan.renderEntryDropNotBuildableCount, 0);
    EXPECT_EQ(plan.renderEntryDropNoReadyTextureCount, 0);
}

TEST(
    TileRenderPlanFinalizerTest,
    FirstBuildBudgetExhaustedUsesAncestorClip) {
    // H-S5:内容已就绪但命令缓存未建(首建待执行)的瓦片,预算耗尽时应改走
    // 祖先裁剪回退(首建顺延),不直接建 —— 摊平扫掠前沿的集中首建 burst,
    // 且祖先覆盖保证不露底。
    const TileKey parentKey{"test", 0, 0, 0};
    const TileKey childKey{"test", 1, 0, 0};
    TilesetTile parent(parentKey, Rectangle{0.0, 0.0, 2.0, 2.0});
    TilesetTile child(childKey, Rectangle{0.0, 1.0, 1.0, 2.0}, &parent);
    makeGltfRenderReady(parent);
    makeGltfRenderReady(child);

    std::unordered_map<std::string, TilesetTile*> tiles{
        {TileCacheKey::forTile(parentKey), &parent},
        {TileCacheKey::forTile(childKey), &child}};

    TilePlan plan;
    plan.visibleTiles.push_back(childKey);
    refreshRenderEntries(
        plan,
        testFinalizeOptions(
            true,   // interactionActive:用交互期首建预算
            0,
            1,
            1.0,
            0,      // activeInteractionFirstBuildBudget = 0 → 耗尽
            8),
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
    EXPECT_EQ(plan.renderEntryFirstBuildDeferredCount, 1);
}

TEST(
    TileRenderPlanFinalizerTest,
    FirstBuildBudgetAvailableAllowsDirectEntry) {
    const TileKey parentKey{"test", 0, 0, 0};
    const TileKey childKey{"test", 1, 0, 0};
    TilesetTile parent(parentKey, Rectangle{0.0, 0.0, 2.0, 2.0});
    TilesetTile child(childKey, Rectangle{0.0, 1.0, 1.0, 2.0}, &parent);
    makeGltfRenderReady(parent);
    makeGltfRenderReady(child);

    std::unordered_map<std::string, TilesetTile*> tiles{
        {TileCacheKey::forTile(parentKey), &parent},
        {TileCacheKey::forTile(childKey), &child}};

    TilePlan plan;
    plan.visibleTiles.push_back(childKey);
    refreshRenderEntries(
        plan,
        testFinalizeOptions(
            true,
            0,
            1,
            1.0,
            1,      // 预算 ≥1 → 本帧直建
            8),
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
    EXPECT_EQ(entry.reason, TileRenderEntryReason::Direct);
    EXPECT_FALSE(entry.usesAncestorFallback);
    EXPECT_FALSE(entry.surfaceClipEnabled);
    EXPECT_EQ(plan.renderEntryAncestorFallbackCount, 0);
    EXPECT_EQ(plan.renderEntryFirstBuildDeferredCount, 0);
}
