#include <gtest/gtest.h>

#include "earth_engine/core/geodesy/Ellipsoid.h"
#include "earth_engine/core/geodesy/QuadtreeGeometricError.h"
#include "earth_engine/core/geodesy/WebMercatorProjection.h"
#include "earth_engine/core/math/MathUtils.h"
#include "earth_engine/content/GltfContentProvider.h"
#include "earth_engine/content/GltfModel.h"
#include "earth_engine/providers/DebugImageryProvider.h"
#include "earth_engine/providers/RasterOverlayTileProvider.h"
#include "earth_engine/renderer/IPrepareRendererResources.h"
#include "earth_engine/renderer/RenderDevice.h"
#include "earth_engine/tiling/RasterMappedToTilesetTile.h"
#include "earth_engine/tiling/TileContentAccess.h"
#include "earth_engine/tiling/TileContentLifecycleManager.h"
#include "earth_engine/tiling/TileLoadState.h"
#include "earth_engine/tiling/TileSelectionRootPolicy.h"
#include "earth_engine/tiling/TileScheme.h"
#include "earth_engine/tiling/TilesetTileRegistry.h"

#include <memory>

using namespace earth_engine;

namespace {

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

class RecordingPrepareRendererResources final
    : public IPrepareRendererResources {
public:
    void attachRasterInMainThread(
        const TileKey& geometryKey,
        int32_t overlayIndex,
        std::shared_ptr<const RasterOverlayTile>,
        Texture*,
        float,
        float,
        float,
        float) override {
        ++attachCount;
        lastGeometryKey = geometryKey;
        lastOverlayIndex = overlayIndex;
    }

    void detachRasterInMainThread(
        const TileKey& geometryKey,
        int32_t overlayIndex) noexcept override {
        ++detachCount;
        lastDetachedGeometryKey = geometryKey;
        lastDetachedOverlayIndex = overlayIndex;
    }

    int attachCount = 0;
    int detachCount = 0;
    TileKey lastGeometryKey;
    TileKey lastDetachedGeometryKey;
    int32_t lastOverlayIndex = -1;
    int32_t lastDetachedOverlayIndex = -1;
};

class ContentOwnedTerrainProvider final : public TilesetContentProvider {
public:
    std::string id() const override { return "content-owned-terrain"; }
    bool supportsTile(const TileKey&) const override { return true; }
    std::vector<TileKey> childTiles(const TileKey& key) const override {
        if (key == TileSelectionRootPolicy::virtualTerrainRootKey(
                       "Geographic-TMS")) {
            return TileSelectionRootPolicy::levelZeroTerrainRoots(
                "Geographic-TMS");
        }
        return {};
    }
    bool providesTerrainQuadtree() const override { return true; }
    TileAvailabilityState availabilityState(
        const TileKey&) const override {
        return TileAvailabilityState::Available;
    }
    void requestTileContent(const TileKey&,
                            CancellationToken,
                            ContentCallback,
                            HttpRequestPriority =
                                HttpRequestPriority::Normal) override {}
    TileContentLoadResult decodeContent(const uint8_t*, size_t) override {
        return TileContentLoadResult::failed();
    }
};

class ContentOwnedWebMercatorTerrainProvider final : public TilesetContentProvider {
public:
    std::string id() const override {
        return "content-owned-web-mercator-terrain";
    }
    bool supportsTile(const TileKey&) const override { return true; }
    std::vector<TileKey> rootTiles() const override {
        return {
            TileSelectionRootPolicy::virtualTerrainRootKey(
                "XYZ-WebMercator")};
    }
    std::vector<TileKey> childTiles(const TileKey& key) const override {
        if (key == TileSelectionRootPolicy::virtualTerrainRootKey(
                       "XYZ-WebMercator")) {
            return {TileKey{"XYZ-WebMercator", 0, 0, 0}};
        }
        return {};
    }
    std::optional<TilesetContentTileMetadata> tileMetadata(
        const TileKey& key) const override {
        if (!TileSelectionRootPolicy::isVirtualTerrainRoot(key) ||
            key.schemeId != "XYZ-WebMercator") {
            return std::nullopt;
        }
        TilesetContentTileMetadata metadata;
        metadata.key = key;
        metadata.bounds = WebMercatorProjection::maximumGlobeRectangle();
        metadata.hasExplicitBounds = true;
        metadata.boundingVolume = TileBoundingVolume::fromLooseRegion(
            metadata.bounds,
            -1000.0,
            9000.0);
        metadata.geometricError = calcLayerJsonTerrainGeometricError(
            Ellipsoid::WGS84(),
            metadata.bounds);
        metadata.refine = TileRefine::Replace;
        metadata.unconditionallyRefine = true;
        return metadata;
    }
    bool providesTerrainQuadtree() const override { return true; }
    TileAvailabilityState availabilityState(
        const TileKey&) const override {
        return TileAvailabilityState::Available;
    }
    void requestTileContent(const TileKey&,
                            CancellationToken,
                            ContentCallback,
                            HttpRequestPriority =
                                HttpRequestPriority::Normal) override {}
    TileContentLoadResult decodeContent(const uint8_t*, size_t) override {
        return TileContentLoadResult::failed();
    }
};

class ProviderOrderedRootTerrainProvider final : public TilesetContentProvider {
public:
    std::string id() const override {
        return "provider-ordered-root-terrain";
    }
    bool supportsTile(const TileKey&) const override { return true; }
    std::vector<TileKey> childTiles(const TileKey& key) const override {
        if (key == TileSelectionRootPolicy::virtualTerrainRootKey(
                       "Geographic-TMS")) {
            return {
                TileKey{"Geographic-TMS", 0, 1, 0},
                TileKey{"Geographic-TMS", 0, 0, 0}};
        }
        return {};
    }
    bool providesTerrainQuadtree() const override { return true; }
    TileAvailabilityState availabilityState(
        const TileKey&) const override {
        return TileAvailabilityState::Available;
    }
    void requestTileContent(const TileKey&,
                            CancellationToken,
                            ContentCallback,
                            HttpRequestPriority =
                                HttpRequestPriority::Normal) override {}
    TileContentLoadResult decodeContent(const uint8_t*, size_t) override {
        return TileContentLoadResult::failed();
    }
};

struct GeographicRootFixture {
    TilesetTileRegistry registry;
    std::unique_ptr<TileScheme> scheme = TileScheme::createGeographicTMS();
    TileContentLifecycleManager lifecycle;
    TileContentAccess contentAccess =
        TileContentAccess::forNoTerrain(
            registry,
            *scheme,
            nullptr);
};

struct WebMercatorRootFixture {
    TilesetTileRegistry registry;
    std::unique_ptr<TileScheme> scheme = TileScheme::createXYZWebMercator();
    TileContentLifecycleManager lifecycle;
    TileContentAccess contentAccess =
        TileContentAccess::forNoTerrain(
            registry,
            *scheme,
            nullptr);
};

struct ContentOwnedGeographicRootFixture {
    TilesetTileRegistry registry;
    std::unique_ptr<TileScheme> scheme = TileScheme::createGeographicTMS();
    TileContentLifecycleManager lifecycle;
    ContentOwnedTerrainProvider provider;
    TileContentAccess contentAccess =
        TileContentAccess::forContentTerrain(
        registry,
        *scheme,
        provider);
};

struct ContentOwnedWebMercatorRootFixture {
    TilesetTileRegistry registry;
    std::unique_ptr<TileScheme> scheme = TileScheme::createXYZWebMercator();
    ContentOwnedWebMercatorTerrainProvider provider;
    TileContentAccess contentAccess =
        TileContentAccess::forContentTerrain(
        registry,
        *scheme,
        provider);
};

struct ProviderOrderedRootFixture {
    TilesetTileRegistry registry;
    std::unique_ptr<TileScheme> scheme = TileScheme::createGeographicTMS();
    ProviderOrderedRootTerrainProvider provider;
    TileContentAccess contentAccess =
        TileContentAccess::forContentTerrain(
            registry,
            *scheme,
            provider);
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

} // namespace

TEST(TileSelectionRootPolicyTest, ExplicitContentRootsTakePriority) {
    const std::vector<TileKey> explicitRoots{
        TileKey{"content", 2, 4, 6},
        TileKey{"content", 3, 5, 7},
    };

    EXPECT_EQ(
        TileSelectionRootPolicy::chooseRoots(
            "Geographic-TMS",
            explicitRoots,
            true),
        explicitRoots);
}

TEST(TileSelectionRootPolicyTest, GeographicTmsUsesVirtualTerrainRoot) {
    const std::vector<TileKey> roots =
        TileSelectionRootPolicy::chooseRoots("Geographic-TMS", {}, true);

    ASSERT_EQ(roots.size(), 1u);
    EXPECT_EQ(
        roots[0],
        TileSelectionRootPolicy::virtualTerrainRootKey("Geographic-TMS"));
    EXPECT_TRUE(TileSelectionRootPolicy::isVirtualTerrainRoot(roots[0]));
}

TEST(TileSelectionRootPolicyTest, GeographicTmsWithoutTerrainDomainUsesDataRoots) {
    const std::vector<TileKey> roots =
        TileSelectionRootPolicy::chooseRoots("Geographic-TMS", {}, false);

    ASSERT_EQ(roots.size(), 2u);
    EXPECT_EQ(roots[0], (TileKey{"Geographic-TMS", 0, 0, 0}));
    EXPECT_EQ(roots[1], (TileKey{"Geographic-TMS", 0, 1, 0}));
}

TEST(TileSelectionRootPolicyTest, GeographicTmsLevelZeroDataRootsStayExplicit) {
    const std::vector<TileKey> roots =
        TileSelectionRootPolicy::levelZeroTerrainRoots("Geographic-TMS");

    ASSERT_EQ(roots.size(), 2u);
    EXPECT_EQ(roots[0], (TileKey{"Geographic-TMS", 0, 0, 0}));
    EXPECT_EQ(roots[1], (TileKey{"Geographic-TMS", 0, 1, 0}));
}

TEST(TileSelectionRootPolicyTest, WebMercatorUsesVirtualTerrainRoot) {
    const std::vector<TileKey> roots =
        TileSelectionRootPolicy::chooseRoots("XYZ-WebMercator", {}, true);

    ASSERT_EQ(roots.size(), 1u);
    EXPECT_EQ(
        roots[0],
        TileSelectionRootPolicy::virtualTerrainRootKey("XYZ-WebMercator"));
    EXPECT_TRUE(TileSelectionRootPolicy::isVirtualTerrainRoot(roots[0]));
}

TEST(TileSelectionRootPolicyTest, WebMercatorWithoutTerrainDomainUsesDataRoot) {
    const std::vector<TileKey> roots =
        TileSelectionRootPolicy::chooseRoots("XYZ-WebMercator", {}, false);

    ASSERT_EQ(roots.size(), 1u);
    EXPECT_EQ(roots[0], (TileKey{"XYZ-WebMercator", 0, 0, 0}));
    EXPECT_FALSE(TileSelectionRootPolicy::isVirtualTerrainRoot(roots[0]));
}

TEST(TileSelectionRootPolicyTest, OpenGlobusEarthUsesThreeRoots) {
    const std::vector<TileKey> roots =
        TileSelectionRootPolicy::chooseRoots("OpenGlobus-Earth", {}, true);

    ASSERT_EQ(roots.size(), 3u);
    EXPECT_EQ(roots[0], (TileKey{"OpenGlobus-Earth", 0, 0, 0}));
    EXPECT_EQ(roots[1], (TileKey{"OpenGlobus-Earth", 0, 0, 1}));
    EXPECT_EQ(roots[2], (TileKey{"OpenGlobus-Earth", 0, 0, 2}));
}

TEST(TileSelectionRootPolicyTest, UnknownSchemeUsesOneDefaultRoot) {
    const std::vector<TileKey> roots =
        TileSelectionRootPolicy::chooseRoots("custom", {}, true);

    ASSERT_EQ(roots.size(), 1u);
    EXPECT_EQ(roots[0], (TileKey{"custom", 0, 0, 0}));
}

TEST(TileSelectionRootPolicyTest, VirtualTerrainRootIsEmptyDoneRefineNode) {
    GeographicRootFixture fixture;
    const TileKey rootKey =
        TileSelectionRootPolicy::virtualTerrainRootKey("Geographic-TMS");

    TilesetTile* root = fixture.contentAccess.ensureTile(rootKey);

    ASSERT_NE(root, nullptr);
    EXPECT_EQ(root->key, rootKey);
    EXPECT_EQ(root->bounds, Rectangle::MAXIMUM);
    EXPECT_EQ(root->content.loadState, TileLoadState::Done);
    EXPECT_EQ(root->content.contentKind, TileContentKind::Empty);
    EXPECT_TRUE(root->unconditionallyRefine);
    EXPECT_EQ(root->refine, TileRefine::Replace);
    EXPECT_NEAR(root->bounds.west(), -MathUtils::OnePi, 1e-12);
    EXPECT_NEAR(root->bounds.south(), -MathUtils::PiOverTwo, 1e-12);
    EXPECT_NEAR(root->bounds.east(), MathUtils::OnePi, 1e-12);
    EXPECT_NEAR(root->bounds.north(), MathUtils::PiOverTwo, 1e-12);
    ASSERT_TRUE(root->boundingVolume.has_value());
    EXPECT_EQ(root->boundingVolume->kind, TileBoundingVolumeKind::Region);
    EXPECT_NEAR(root->boundingVolume->region.west(), -MathUtils::OnePi, 1e-12);
    EXPECT_NEAR(
        root->boundingVolume->region.south(),
        -MathUtils::PiOverTwo,
        1e-12);
    EXPECT_NEAR(root->boundingVolume->region.east(), MathUtils::OnePi, 1e-12);
    EXPECT_NEAR(
        root->boundingVolume->region.north(),
        MathUtils::PiOverTwo,
        1e-12);
    EXPECT_DOUBLE_EQ(root->boundingVolume->minimumHeight, -1000.0);
    EXPECT_DOUBLE_EQ(root->boundingVolume->maximumHeight, 9000.0);
    EXPECT_TRUE(root->boundingVolume->looseFittingHeights);
    EXPECT_FALSE(root->contentBoundingVolume.has_value());
    EXPECT_EQ(root->rasterOverlayState.mappings().size(), 0u);
}

TEST(TileSelectionRootPolicyTest, VirtualGeographicRootLinksLevelZeroDataTiles) {
    GeographicRootFixture fixture;
    TilesetTile* root = fixture.contentAccess.ensureTile(
        TileSelectionRootPolicy::virtualTerrainRootKey("Geographic-TMS"));

    fixture.contentAccess.ensureTileChildren(*root);
    fixture.contentAccess.ensureTileChildren(*root);

    ASSERT_EQ(root->children.size(), 2u);
    EXPECT_EQ(root->children[0]->key, (TileKey{"Geographic-TMS", 0, 0, 0}));
    EXPECT_EQ(root->children[1]->key, (TileKey{"Geographic-TMS", 0, 1, 0}));
    EXPECT_EQ(root->children[0]->parent, root);
    EXPECT_EQ(root->children[1]->parent, root);
    EXPECT_EQ(root->children[0]->content.loadState, TileLoadState::Unloaded);
    EXPECT_EQ(root->children[1]->content.loadState, TileLoadState::Unloaded);
    EXPECT_FALSE(root->children[0]->unconditionallyRefine);
    EXPECT_FALSE(root->children[1]->unconditionallyRefine);

    const TilesetTile& west = *root->children[0];
    const TilesetTile& east = *root->children[1];
    EXPECT_NEAR(616538.71824, west.geometricError, 1e-5);
    EXPECT_NEAR(616538.71824, east.geometricError, 1e-5);
    EXPECT_NEAR(west.bounds.west(), -MathUtils::OnePi, 1e-12);
    EXPECT_NEAR(west.bounds.south(), -MathUtils::PiOverTwo, 1e-12);
    EXPECT_NEAR(west.bounds.east(), 0.0, 1e-12);
    EXPECT_NEAR(west.bounds.north(), MathUtils::PiOverTwo, 1e-12);
    EXPECT_NEAR(east.bounds.west(), 0.0, 1e-12);
    EXPECT_NEAR(east.bounds.south(), -MathUtils::PiOverTwo, 1e-12);
    EXPECT_NEAR(east.bounds.east(), MathUtils::OnePi, 1e-12);
    EXPECT_NEAR(east.bounds.north(), MathUtils::PiOverTwo, 1e-12);
    for (const TilesetTile* child : root->children) {
        ASSERT_TRUE(child->boundingVolume.has_value());
        EXPECT_EQ(child->boundingVolume->kind, TileBoundingVolumeKind::Region);
        EXPECT_DOUBLE_EQ(child->boundingVolume->minimumHeight, -1000.0);
        EXPECT_DOUBLE_EQ(child->boundingVolume->maximumHeight, 9000.0);
        EXPECT_TRUE(child->boundingVolume->looseFittingHeights);
        EXPECT_FALSE(child->contentBoundingVolume.has_value());
        EXPECT_TRUE(child->content.renderContent.hasTerrainHeightRange());
        EXPECT_DOUBLE_EQ(
            child->content.renderContent.terrainMinimumHeight(),
            -1000.0);
        EXPECT_DOUBLE_EQ(
            child->content.renderContent.terrainMaximumHeight(),
            9000.0);
    }
}

TEST(TileSelectionRootPolicyTest,
     ContentOwnedVirtualRootUsesProviderChildrenLikeLayerJsonTerrainLoader) {
    ProviderOrderedRootFixture fixture;
    TilesetTile* root = fixture.contentAccess.ensureTile(
        TileSelectionRootPolicy::virtualTerrainRootKey("Geographic-TMS"));
    ASSERT_NE(nullptr, root);

    fixture.contentAccess.ensureTileChildren(*root);

    ASSERT_EQ(2u, root->children.size());
    EXPECT_EQ((TileKey{"Geographic-TMS", 0, 1, 0}),
              root->children[0]->key);
    EXPECT_EQ((TileKey{"Geographic-TMS", 0, 0, 0}),
              root->children[1]->key);
    EXPECT_EQ(root, root->children[0]->parent);
    EXPECT_EQ(root, root->children[1]->parent);
}

TEST(TileSelectionRootPolicyTest,
     ContentOwnedVirtualRootClearsLegacyResidueFromLevelZeroTiles) {
    ContentOwnedGeographicRootFixture fixture;
    const TileKey levelZeroKey{"Geographic-TMS", 0, 1, 0};
    TilesetTile* levelZero = fixture.contentAccess.ensureTile(levelZeroKey);
    ASSERT_NE(nullptr, levelZero);
    auto gltfModelClear = makeQuadTerrainGltfModel(levelZero->bounds);
    levelZero->content.renderContent.prepareGltfContent(
        std::move(gltfModelClear), Mat4::identity());
    levelZero->content.renderContent.setTerrainRenderContent(true);
    levelZero->content.renderContent.addGltfPrimitiveResource(
        GltfPrimitiveRenderResources{});
    levelZero->content.renderContent.markRenderContentReady();
    levelZero->content.renderContent.setRetainedHeightmap(
        std::make_unique<DecodedHeightmap>());
    levelZero->rasterOverlayState.ensureMapping(0);
    levelZero->rasterOverlayState.missingProjections().push_back(
        RasterOverlayProjection{});

    TilesetTile* root = fixture.contentAccess.ensureTile(
        TileSelectionRootPolicy::virtualTerrainRootKey("Geographic-TMS"));
    ASSERT_NE(nullptr, root);

    fixture.contentAccess.ensureTileChildren(*root);

    ASSERT_EQ(2u, root->children.size());
    EXPECT_EQ(levelZero, root->children[1]);
    EXPECT_FALSE(levelZero->content.renderContent.hasRetainedHeightmap());
    EXPECT_FALSE(levelZero->content.renderContent.isRenderContentReady());
    EXPECT_EQ(0u, levelZero->rasterOverlayState.mappingCount());
    EXPECT_FALSE(levelZero->rasterOverlayState.hasMissingProjections());
    EXPECT_TRUE(levelZero->content.renderContent.hasTerrainHeightRange());
    EXPECT_DOUBLE_EQ(
        -1000.0,
        levelZero->content.renderContent.terrainMinimumHeight());
    EXPECT_DOUBLE_EQ(
        9000.0,
        levelZero->content.renderContent.terrainMaximumHeight());
}

TEST(TileSelectionRootPolicyTest,
     ContentOwnedVirtualRootDetachesStaleRasterResidueLikeCesiumNative) {
    ContentOwnedGeographicRootFixture fixture;
    const TileKey levelZeroKey{"Geographic-TMS", 0, 1, 0};
    TilesetTile* levelZero = fixture.contentAccess.ensureTile(levelZeroKey);
    ASSERT_NE(nullptr, levelZero);
    auto gltfModelDetach = makeQuadTerrainGltfModel(levelZero->bounds);
    levelZero->content.renderContent.prepareGltfContent(
        std::move(gltfModelDetach), Mat4::identity());
    levelZero->content.renderContent.setTerrainRenderContent(true);
    levelZero->content.renderContent.addGltfPrimitiveResource(
        GltfPrimitiveRenderResources{});
    levelZero->content.renderContent.markRenderContentReady();
    levelZero->content.renderContent.mutableRasterOverlayDetails()
        ->setGeographicRectangle(levelZero->bounds);

    DebugImageryProvider imagery;
    auto scheme = TileScheme::createGeographicTMS();
    RasterOverlayTileProvider rasterProvider(imagery, *scheme, nullptr);
    RasterMappedToTilesetTile& mapped =
        levelZero->rasterOverlayState.ensureMapping(0);
    std::vector<RasterOverlayProjection> missingProjections;
    mapped.update(
        levelZero->key,
        levelZero->content.renderContent.rasterOverlayDetails(),
        256.0,
        256.0,
        rasterProvider,
        nullptr,
        missingProjections);
    ASSERT_NE(nullptr, mapped.getLoadingTile());
    mapped.getLoadingTile()->setTexture(
        std::make_unique<DummyTexture>(4, 4));
    RecordingPrepareRendererResources prep;
    mapped.update(
        levelZero->key,
        levelZero->content.renderContent.rasterOverlayDetails(),
        256.0,
        256.0,
        rasterProvider,
        &prep,
        missingProjections);
    ASSERT_EQ(RasterMappedToTilesetTile::State::Attached,
              mapped.getState());
    ASSERT_EQ(1, prep.attachCount);

    TilesetTile* root = fixture.contentAccess.ensureTile(
        TileSelectionRootPolicy::virtualTerrainRootKey("Geographic-TMS"));
    ASSERT_NE(nullptr, root);

    fixture.contentAccess.ensureTileChildren(*root, &prep);

    ASSERT_EQ(2u, root->children.size());
    EXPECT_EQ(levelZero, root->children[1]);
    EXPECT_EQ(1, prep.detachCount);
    EXPECT_EQ(levelZeroKey, prep.lastDetachedGeometryKey);
    EXPECT_EQ(0, prep.lastDetachedOverlayIndex);
    EXPECT_EQ(0u, levelZero->rasterOverlayState.mappingCount());
    EXPECT_FALSE(levelZero->content.renderContent.isRenderContentReady());
}

TEST(TileSelectionRootPolicyTest,
     ContentOwnedVirtualRootPreservesLoadedGltfLevelZeroTiles) {
    ContentOwnedGeographicRootFixture fixture;
    const TileKey levelZeroKey{"Geographic-TMS", 0, 1, 0};
    TilesetTile* levelZero = fixture.contentAccess.ensureTile(levelZeroKey);
    ASSERT_NE(nullptr, levelZero);
    auto acceptedModel = std::make_unique<GltfModel>();
    acceptedModel->rasterOverlayDetails.setGeographicRectangle(
        levelZero->bounds,
        -25.0,
        125.0);
    levelZero->content.renderContent.prepareGltfContent(
        std::move(acceptedModel),
        Mat4::identity());
    levelZero->content.renderContent.setTerrainRenderContent(true);
    levelZero->content.renderContent.setTerrainHeightRange(-25.0, 125.0);
    levelZero->content.renderContent.addGltfPrimitiveResource(
        GltfPrimitiveRenderResources{});
    levelZero->markRenderContentDone();
    RasterMappedToTilesetTile* existingMapping =
        &levelZero->rasterOverlayState.ensureMapping(0);

    TilesetTile* root = fixture.contentAccess.ensureTile(
        TileSelectionRootPolicy::virtualTerrainRootKey("Geographic-TMS"));
    ASSERT_NE(nullptr, root);

    fixture.contentAccess.ensureTileChildren(*root);

    EXPECT_TRUE(levelZero->content.renderContent.hasGltfModel());
    EXPECT_TRUE(levelZero->content.renderContent.isRenderContentReady());
    ASSERT_TRUE(levelZero->content.renderContent.hasTerrainHeightRange());
    EXPECT_DOUBLE_EQ(
        -25.0,
        levelZero->content.renderContent.terrainMinimumHeight());
    EXPECT_DOUBLE_EQ(
        125.0,
        levelZero->content.renderContent.terrainMaximumHeight());
    EXPECT_EQ(existingMapping, levelZero->rasterOverlayState.mappingAt(0));
}

TEST(
    TileSelectionRootPolicyTest,
    ContentOwnedWebMercatorVirtualRootAppliesProviderMetadataOnCreation) {
    ContentOwnedWebMercatorRootFixture fixture;
    const TileKey rootKey =
        TileSelectionRootPolicy::virtualTerrainRootKey("XYZ-WebMercator");

    TilesetTile* root = fixture.contentAccess.ensureTile(rootKey);

    ASSERT_NE(root, nullptr);
    const Rectangle expectedBounds =
        WebMercatorProjection::maximumGlobeRectangle();
    EXPECT_EQ(root->key, rootKey);
    EXPECT_TRUE(root->unconditionallyRefine);
    EXPECT_NEAR(root->bounds.west(), expectedBounds.west(), 1e-12);
    EXPECT_NEAR(root->bounds.south(), expectedBounds.south(), 1e-12);
    EXPECT_NEAR(root->bounds.east(), expectedBounds.east(), 1e-12);
    EXPECT_NEAR(root->bounds.north(), expectedBounds.north(), 1e-12);
    EXPECT_NEAR(
        root->geometricError,
        calcLayerJsonTerrainGeometricError(Ellipsoid::WGS84(), expectedBounds),
        1e-5);
    ASSERT_TRUE(root->boundingVolume.has_value());
    EXPECT_EQ(root->boundingVolume->kind, TileBoundingVolumeKind::Region);
    EXPECT_TRUE(root->boundingVolume->looseFittingHeights);
    EXPECT_NEAR(
        root->boundingVolume->region.south(),
        expectedBounds.south(),
        1e-12);
    EXPECT_NEAR(
        root->boundingVolume->region.north(),
        expectedBounds.north(),
        1e-12);
}

TEST(TileSelectionRootPolicyTest, VirtualWebMercatorRootLinksLevelZeroDataTile) {
    WebMercatorRootFixture fixture;
    TilesetTile* root = fixture.contentAccess.ensureTile(
        TileSelectionRootPolicy::virtualTerrainRootKey("XYZ-WebMercator"));

    fixture.contentAccess.ensureTileChildren(*root);
    fixture.contentAccess.ensureTileChildren(*root);

    ASSERT_EQ(root->children.size(), 1u);
    EXPECT_EQ(root->children[0]->key, (TileKey{"XYZ-WebMercator", 0, 0, 0}));
    EXPECT_EQ(root->children[0]->parent, root);
    EXPECT_EQ(root->children[0]->content.loadState, TileLoadState::Unloaded);
    EXPECT_FALSE(root->children[0]->unconditionallyRefine);
    const Rectangle expectedBounds =
        WebMercatorProjection::maximumGlobeRectangle();
    EXPECT_NEAR(
        calcLayerJsonTerrainGeometricError(Ellipsoid::WGS84(), expectedBounds),
        root->children[0]->geometricError,
        1e-5);
    EXPECT_NEAR(root->children[0]->bounds.west(), expectedBounds.west(), 1e-12);
    EXPECT_NEAR(
        root->children[0]->bounds.south(),
        expectedBounds.south(),
        1e-12);
    EXPECT_NEAR(root->children[0]->bounds.east(), expectedBounds.east(), 1e-12);
    EXPECT_NEAR(
        root->children[0]->bounds.north(),
        expectedBounds.north(),
        1e-12);
    ASSERT_TRUE(root->children[0]->boundingVolume.has_value());
    EXPECT_EQ(
        root->children[0]->boundingVolume->kind,
        TileBoundingVolumeKind::Region);
    EXPECT_DOUBLE_EQ(root->children[0]->boundingVolume->minimumHeight, -1000.0);
    EXPECT_DOUBLE_EQ(root->children[0]->boundingVolume->maximumHeight, 9000.0);
    EXPECT_TRUE(root->children[0]->boundingVolume->looseFittingHeights);
    EXPECT_FALSE(root->children[0]->contentBoundingVolume.has_value());
    EXPECT_EQ(root->rasterOverlayState.mappings().size(), 0u);
}
