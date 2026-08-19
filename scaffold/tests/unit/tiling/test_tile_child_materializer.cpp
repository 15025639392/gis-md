#include <gtest/gtest.h>

#include "earth_engine/core/math/MathUtils.h"
#include "earth_engine/content/GltfModel.h"
#include "earth_engine/providers/DebugImageryProvider.h"
#include "earth_engine/providers/RasterOverlayTileProvider.h"
#include "earth_engine/renderer/RenderDevice.h"
#include "earth_engine/core/geodesy/Ellipsoid.h"
#include "earth_engine/core/geodesy/Projection.h"
#include "earth_engine/core/geodesy/WebMercatorProjection.h"
#include "earth_engine/tiling/SurfaceRasterBinding.h"
#include "earth_engine/tiling/SurfaceTile.h"
#include "earth_engine/tiling/RasterMappedToTilesetTile.h"
#include "earth_engine/tiling/RasterOverlayProjection.h"
#include "earth_engine/tiling/TileChildFrameMaterializer.h"
#include "earth_engine/tiling/TileChildMaterializer.h"
#include "earth_engine/tiling/TileGltfTerrainUpsampledChildMaterializer.h"
#include "earth_engine/tiling/TileLoadStatePredicates.h"
#include "earth_engine/tiling/TileRasterUpsampledChildMaterializer.h"
#include "earth_engine/tiling/TileScheme.h"
#include "earth_engine/tiling/TileTerrainHeightRangePolicy.h"
#include "earth_engine/renderer/IPrepareRendererResources.h"

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

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
        std::shared_ptr<const RasterOverlayTile> rasterTile,
        Texture* texture,
        float translationU,
        float translationV,
        float scaleU,
        float scaleV) override {
        ++attachCount;
        lastGeometryKey = geometryKey;
        lastOverlayIndex = overlayIndex;
        lastRasterTile = std::move(rasterTile);
        lastTexture = texture;
        lastUv = {translationU, translationV, scaleU, scaleV};
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
    std::shared_ptr<const RasterOverlayTile> lastRasterTile;
    Texture* lastTexture = nullptr;
    std::array<float, 4> lastUv{0.0f, 0.0f, 1.0f, 1.0f};
};

std::string cacheKeyFor(const TileKey& key) {
    return key.schemeId.str() + ":" +
           std::to_string(key.z) + ":" +
           std::to_string(key.x) + ":" +
           std::to_string(key.y);
}

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

RasterMappedToTilesetTile& addMoreDetailRasterMapping(
    TilesetTile& tile,
    RasterOverlayTileProvider& provider) {
    auto gltfModel = makeQuadTerrainGltfModel(tile.bounds);
    tile.content.renderContent.prepareGltfContent(
        std::move(gltfModel), Mat4::identity());
    tile.content.renderContent.setTerrainRenderContent(true);
    tile.content.renderContent.addGltfPrimitiveResource(
        GltfPrimitiveRenderResources{});
    tile.content.renderContent.markRenderContentReady();

    auto& mapped = tile.rasterOverlayState.ensureMapping(0);
    std::vector<RasterOverlayProjection> missingProjections;
    const RasterMappedToTilesetTile::MoreDetail firstMoreDetail =
        mapped.update(
            tile.key,
            tile.content.renderContent.rasterOverlayDetails(),
            256.0,
            256.0,
            provider,
            nullptr,
            missingProjections);

    RasterOverlayTile* loadingTile = mapped.getLoadingTile();
    RasterOverlayTile* readyTile = mapped.getReadyTile();
    EXPECT_TRUE(
        firstMoreDetail == RasterMappedToTilesetTile::MoreDetail::Unknown ||
        loadingTile != nullptr ||
        readyTile != nullptr);
    if (loadingTile) {
        loadingTile->setState(RasterOverlayTile::LoadState::Loaded);
        loadingTile->setMoreDetailAvailable(
            RasterOverlayTile::MoreDetailAvailable::Yes);
    } else {
        EXPECT_NE(nullptr, readyTile);
        if (!readyTile) {
            return mapped;
        }
        readyTile->setMoreDetailAvailable(
            RasterOverlayTile::MoreDetailAvailable::Yes);
    }

    EXPECT_EQ(
        RasterMappedToTilesetTile::MoreDetail::Yes,
        mapped.update(
            tile.key,
            tile.content.renderContent.rasterOverlayDetails(),
            256.0,
            256.0,
            provider,
            nullptr,
            missingProjections));
    return mapped;
}

} // namespace

TEST(TileChildMaterializerTest, LinkContentChildrenWithoutDuplicates) {
    const TileKey parentKey{"test", 0, 0, 0};
    const TileKey firstKey{"test", 1, 0, 0};
    const TileKey secondKey{"test", 1, 1, 0};
    TilesetTile parent(parentKey, Rectangle{});

    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;
    tiles.emplace(
        "test:1:0:0",
        std::make_unique<TilesetTile>(firstKey, Rectangle{}));
    tiles.emplace(
        "test:1:1:0",
        std::make_unique<TilesetTile>(secondKey, Rectangle{}));

    auto ensure = [&tiles](const TileKey& key) -> TilesetTile* {
        auto it = tiles.find(cacheKeyFor(key));
        return it == tiles.end() ? nullptr : it->second.get();
    };
    const std::vector<TileKey> childKeys{firstKey, secondKey, firstKey};

    const bool changed =
        TileChildMaterializer::linkContentChildren(parent, childKeys, ensure);
    const bool changedAgain =
        TileChildMaterializer::linkContentChildren(parent, childKeys, ensure);

    EXPECT_TRUE(changed);
    EXPECT_FALSE(changedAgain);
    ASSERT_EQ(2u, parent.children.size());
    EXPECT_EQ(tiles["test:1:0:0"].get(), parent.children[0]);
    EXPECT_EQ(tiles["test:1:1:0"].get(), parent.children[1]);
    EXPECT_EQ(&parent, parent.children[0]->parent);
    EXPECT_EQ(&parent, parent.children[1]->parent);
}

TEST(TileChildMaterializerTest, GltfUpsampleClipInputDropsTransientMeshCopies) {
    const Rectangle parentBounds{-1.0, -0.5, 1.0, 0.5};
    const Rectangle childBounds{-1.0, -0.5, 0.0, 0.0};
    TilesetTile parent(TileKey{"test", 0, 0, 0}, parentBounds);
    TilesetTile child(TileKey{"test", 1, 0, 0}, childBounds, &parent);
    child.content.markTerrainAvailabilityUpsample();

    auto parentModel = makeQuadTerrainGltfModel(parentBounds);
    ASSERT_FALSE(parentModel->primitives.empty());
    GltfPrimitive& sourcePrimitive = parentModel->primitives.front();
    sourcePrimitive.terrainGpuVertexBytes.resize(4096, 3);
    sourcePrimitive.instances.resize(8);
    sourcePrimitive.runtime.baseTangents.resize(sourcePrimitive.vertices.size());
    sourcePrimitive.runtime.skinning.resize(sourcePrimitive.vertices.size());
    sourcePrimitive.runtime.morphTargets.resize(2);
    const size_t retainedBaseVertexCount =
        sourcePrimitive.runtime.baseVertices.size();
    parent.content.renderContent.prepareGltfContent(
        std::move(parentModel), Mat4::identity());
    parent.content.renderContent.setTerrainRenderContent(true);
    parent.content.loadState = TileLoadState::Done;

    std::optional<
        TileGltfTerrainUpsampledChildMaterializer::UpsampleClipInput>
        input = TileGltfTerrainUpsampledChildMaterializer::buildClipInput(
            child);

    ASSERT_TRUE(input.has_value());
    ASSERT_NE(nullptr, input->parentModel);
    ASSERT_FALSE(input->parentModel->primitives.empty());
    const GltfPrimitive& snapshotPrimitive =
        input->parentModel->primitives.front();
    EXPECT_TRUE(snapshotPrimitive.terrainGpuVertexBytes.empty());
    EXPECT_TRUE(snapshotPrimitive.instances.empty());
    EXPECT_TRUE(snapshotPrimitive.runtime.baseVertices.empty());
    EXPECT_TRUE(snapshotPrimitive.runtime.baseTangents.empty());
    EXPECT_TRUE(snapshotPrimitive.runtime.skinning.empty());
    EXPECT_TRUE(snapshotPrimitive.runtime.morphTargets.empty());
    EXPECT_EQ(4u, snapshotPrimitive.vertices.size());
    EXPECT_EQ(6u, snapshotPrimitive.indices.size());
    EXPECT_EQ(4u, snapshotPrimitive.vertexTexCoords[0].size());
    const GltfModel* retainedModel =
        parent.content.renderContent.gltfModelForRead();
    ASSERT_NE(nullptr, retainedModel);
    ASSERT_FALSE(retainedModel->primitives.empty());
    // The snapshot is born pruned via a steal-copy-restore of the parent's
    // runtime payloads — every stolen field must be back untouched.
    const GltfPrimitive& retainedPrimitive =
        retainedModel->primitives.front();
    EXPECT_EQ(
        retainedBaseVertexCount,
        retainedPrimitive.runtime.baseVertices.size());
    EXPECT_EQ(4096u, retainedPrimitive.terrainGpuVertexBytes.size());
    EXPECT_EQ(3, retainedPrimitive.terrainGpuVertexBytes.front());
    EXPECT_EQ(8u, retainedPrimitive.instances.size());
    EXPECT_EQ(
        retainedPrimitive.vertices.size(),
        retainedPrimitive.runtime.baseTangents.size());
    EXPECT_EQ(
        retainedPrimitive.vertices.size(),
        retainedPrimitive.runtime.skinning.size());
    EXPECT_EQ(2u, retainedPrimitive.runtime.morphTargets.size());

    std::unique_ptr<GltfModel> childModel =
        TileGltfTerrainUpsampledChildMaterializer::clipToModel(*input);
    ASSERT_NE(nullptr, childModel);
    ASSERT_FALSE(childModel->primitives.empty());
    EXPECT_EQ(
        childModel->primitives.front().vertices.size(),
        childModel->primitives.front().runtime.baseVertices.size());
}

TEST(TileChildMaterializerTest, AnyAvailableTerrainChildCreatesFullQuadLikeCesiumNative) {
    auto scheme = TileScheme::createGeographicTMS();
    TilesetTile parent(
        TileKey{"Geographic-TMS", 0, 0, 0},
        scheme->tileToRectangle(TileKey{"Geographic-TMS", 0, 0, 0}));
    parent.geometricError = 100.0;
    parent.refine = TileRefine::Add;
    parent.content.renderContent.setTerrainHeightRange(-10.0, 90.0);

    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;
    auto ensure = [&tiles, &scheme](const TileKey& key) -> TilesetTile* {
        const std::string cacheKey = cacheKeyFor(key);
        auto it = tiles.find(cacheKey);
        if (it == tiles.end()) {
            it = tiles.emplace(
                cacheKey,
                std::make_unique<TilesetTile>(
                    key,
                    scheme->tileToRectangle(key)))
                     .first;
        }
        return it->second.get();
    };
    auto availability = [](const TileKey& key) {
        return key.x == 0 && key.y == 0
            ? TileAvailabilityState::Available
            : TileAvailabilityState::NotAvailable;
    };

    const bool changed = TileChildMaterializer::materializeTerrainChildren(
        parent,
        2,
        availability,
        ensure);

    ASSERT_TRUE(changed);
    ASSERT_EQ(4u, parent.children.size());

    TilesetTile* sw = parent.children[0];
    TilesetTile* se = parent.children[1];
    TilesetTile* nw = parent.children[2];
    TilesetTile* ne = parent.children[3];

    EXPECT_FALSE(sw->content.derivesTerrainFromParent());
    EXPECT_TRUE(se->content.derivesTerrainFromParent());
    EXPECT_TRUE(nw->content.derivesTerrainFromParent());
    EXPECT_TRUE(ne->content.derivesTerrainFromParent());

    EXPECT_DOUBLE_EQ(50.0, sw->geometricError);
    EXPECT_DOUBLE_EQ(50.0, se->geometricError);
    EXPECT_EQ(TileRefine::Add, sw->refine);
    EXPECT_EQ(TileRefine::Add, se->refine);
    EXPECT_EQ((TileKey{"Geographic-TMS", 1, 0, 0}), sw->key);
    EXPECT_EQ((TileKey{"Geographic-TMS", 1, 1, 0}), se->key);
    EXPECT_EQ((TileKey{"Geographic-TMS", 1, 0, 1}), nw->key);
    EXPECT_EQ((TileKey{"Geographic-TMS", 1, 1, 1}), ne->key);
    EXPECT_NEAR(-MathUtils::OnePi, sw->bounds.west(), 1e-9);
    EXPECT_NEAR(-MathUtils::PiOverTwo, sw->bounds.south(), 1e-9);
    EXPECT_NEAR(-MathUtils::PiOverTwo, sw->bounds.east(), 1e-9);
    EXPECT_NEAR(0.0, sw->bounds.north(), 1e-9);
    EXPECT_NEAR(-MathUtils::PiOverTwo, ne->bounds.west(), 1e-9);
    EXPECT_NEAR(0.0, ne->bounds.south(), 1e-9);
    EXPECT_NEAR(0.0, ne->bounds.east(), 1e-9);
    EXPECT_NEAR(MathUtils::PiOverTwo, ne->bounds.north(), 1e-9);
    for (const TilesetTile* child : parent.children) {
        ASSERT_NE(nullptr, child);
        ASSERT_TRUE(child->content.renderContent.hasTerrainHeightRange());
        EXPECT_DOUBLE_EQ(
            -10.0,
            child->content.renderContent.terrainMinimumHeight());
        EXPECT_DOUBLE_EQ(
            90.0,
            child->content.renderContent.terrainMaximumHeight());
        ASSERT_TRUE(child->boundingVolume.has_value());
        EXPECT_EQ(TileBoundingVolumeKind::Region, child->boundingVolume->kind);
        EXPECT_DOUBLE_EQ(-10.0, child->boundingVolume->minimumHeight);
        EXPECT_DOUBLE_EQ(90.0, child->boundingVolume->maximumHeight);
        EXPECT_FALSE(child->contentBoundingVolume.has_value());
    }
}

TEST(TileChildMaterializerTest,
     AcceptedGltfTerrainChildKeepsTileLoadResultBounds) {
    auto scheme = TileScheme::createGeographicTMS();
    const TileKey parentKey{"Geographic-TMS", 0, 0, 0};
    TilesetTile parent(parentKey, scheme->tileToRectangle(parentKey));
    parent.geometricError = 100.0;
    parent.refine = TileRefine::Replace;
    parent.content.renderContent.setTerrainHeightRange(-1000.0, 9000.0);

    const TileKey acceptedChildKey{"Geographic-TMS", 1, 1, 0};
    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;
    auto acceptedChild = std::make_unique<TilesetTile>(
        acceptedChildKey,
        scheme->tileToRectangle(acceptedChildKey));
    TilesetTile* acceptedChildRaw = acceptedChild.get();
    tiles.emplace(cacheKeyFor(acceptedChildKey), std::move(acceptedChild));

    const Rectangle tightRectangle =
        Rectangle::fromDegrees(-70.0, -40.0, -60.0, -30.0);
    const Rectangle tightContentRectangle =
        Rectangle::fromDegrees(-69.0, -39.0, -61.0, -31.0);
    acceptedChildRaw->boundingVolume =
        TileBoundingVolume::fromRegion(tightRectangle, -25.0, 125.0);
    acceptedChildRaw->contentBoundingVolume =
        TileBoundingVolume::fromRegion(tightContentRectangle, -10.0, 90.0);
    auto model = std::make_unique<GltfModel>();
    model->rasterOverlayDetails.setGeographicRectangle(
        tightRectangle,
        -25.0,
        125.0);
    acceptedChildRaw->content.renderContent.prepareGltfContent(
        std::move(model),
        Mat4::identity());
    acceptedChildRaw->content.renderContent.setTerrainRenderContent(true);
    acceptedChildRaw->content.renderContent.setTerrainHeightRange(
        -25.0,
        125.0);
    acceptedChildRaw->content.renderContent.addGltfPrimitiveResource(
        GltfPrimitiveRenderResources{});
    acceptedChildRaw->markRenderContentDone();

    auto ensure = [&tiles, &scheme](const TileKey& key) -> TilesetTile* {
        const std::string cacheKey = cacheKeyFor(key);
        auto it = tiles.find(cacheKey);
        if (it == tiles.end()) {
            it = tiles.emplace(
                cacheKey,
                std::make_unique<TilesetTile>(
                    key,
                    scheme->tileToRectangle(key)))
                     .first;
        }
        return it->second.get();
    };
    auto availability = [](const TileKey& key) {
        return key.x == 0 && key.y == 0
            ? TileAvailabilityState::Available
            : TileAvailabilityState::NotAvailable;
    };

    const bool changed = TileChildMaterializer::materializeTerrainChildren(
        parent,
        2,
        availability,
        ensure,
        true);

    EXPECT_TRUE(changed);
    ASSERT_EQ(4u, parent.children.size());
    EXPECT_EQ(acceptedChildRaw, parent.children[1]);
    EXPECT_FALSE(acceptedChildRaw->content.isTerrainAvailabilityUpsample());
    ASSERT_TRUE(acceptedChildRaw->boundingVolume.has_value());
    EXPECT_EQ(tightRectangle, acceptedChildRaw->boundingVolume->region);
    EXPECT_DOUBLE_EQ(-25.0, acceptedChildRaw->boundingVolume->minimumHeight);
    EXPECT_DOUBLE_EQ(125.0, acceptedChildRaw->boundingVolume->maximumHeight);
    EXPECT_FALSE(acceptedChildRaw->boundingVolume->looseFittingHeights);
    ASSERT_TRUE(acceptedChildRaw->contentBoundingVolume.has_value());
    EXPECT_EQ(
        tightContentRectangle,
        acceptedChildRaw->contentBoundingVolume->region);
    EXPECT_DOUBLE_EQ(
        -10.0,
        acceptedChildRaw->contentBoundingVolume->minimumHeight);
    EXPECT_DOUBLE_EQ(
        90.0,
        acceptedChildRaw->contentBoundingVolume->maximumHeight);
    ASSERT_TRUE(
        acceptedChildRaw->content.renderContent.hasTerrainHeightRange());
    EXPECT_DOUBLE_EQ(
        -25.0,
        acceptedChildRaw->content.renderContent.terrainMinimumHeight());
    EXPECT_DOUBLE_EQ(
        125.0,
        acceptedChildRaw->content.renderContent.terrainMaximumHeight());
}

TEST(TileChildMaterializerTest,
     AvailabilityBoundaryWaitsForContentBeforeCreatingChildrenLikeCesiumNative) {
    auto scheme = TileScheme::createGeographicTMS();
    const TileKey parentKey{"Geographic-TMS", 0, 0, 0};
    TilesetTile parent(parentKey, scheme->tileToRectangle(parentKey));
    parent.geometricError = 100.0;
    parent.refine = TileRefine::Replace;

    auto availability = [](const TileKey& key) {
        return key.z == 1 && key.x == 0 && key.y == 0
            ? TileAvailabilityState::Available
            : TileAvailabilityState::NotAvailable;
    };

    for (TileLoadState waitingState : {
             TileLoadState::Unloading,
             TileLoadState::FailedTemporarily,
             TileLoadState::Unloaded,
             TileLoadState::ContentLoading}) {
        parent.clearChildren();
        parent.content.loadState = waitingState;
        std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;
        int ensureCalls = 0;
        auto ensure = [&tiles, &scheme, &ensureCalls](
                          const TileKey& key) -> TilesetTile* {
            ++ensureCalls;
            const std::string cacheKey = cacheKeyFor(key);
            auto it = tiles.find(cacheKey);
            if (it == tiles.end()) {
                it = tiles.emplace(
                    cacheKey,
                    std::make_unique<TilesetTile>(
                        key,
                        scheme->tileToRectangle(key)))
                         .first;
            }
            return it->second.get();
        };

        const TileChildFrameMaterializeResult result =
            TileChildFrameMaterializer::ensureChildren(
            TileChildFrameMaterializeInput{
                parent,
                {},
                2,
                true,
                true},
            ensure,
            availability);

        EXPECT_FALSE(result.changed);
        EXPECT_TRUE(result.retryLater);
        EXPECT_EQ(0, ensureCalls);
        EXPECT_TRUE(parent.children.empty());
    }

    for (TileLoadState resolvedState : {
             TileLoadState::ContentLoaded,
             TileLoadState::Done,
             TileLoadState::Failed}) {
        parent.clearChildren();
        parent.content.loadState = resolvedState;
        std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;
        auto ensure = [&tiles, &scheme](const TileKey& key) -> TilesetTile* {
            const std::string cacheKey = cacheKeyFor(key);
            auto it = tiles.find(cacheKey);
            if (it == tiles.end()) {
                it = tiles.emplace(
                    cacheKey,
                    std::make_unique<TilesetTile>(
                        key,
                        scheme->tileToRectangle(key)))
                         .first;
            }
            return it->second.get();
        };

        const TileChildFrameMaterializeResult result =
            TileChildFrameMaterializer::ensureChildren(
            TileChildFrameMaterializeInput{
                parent,
                {},
                2,
                true,
                false},
            ensure,
            availability);

        EXPECT_TRUE(result.changed);
        EXPECT_FALSE(result.retryLater);
        ASSERT_EQ(4u, parent.children.size());
        EXPECT_EQ((TileKey{"Geographic-TMS", 1, 0, 0}),
                  parent.children[0]->key);
        EXPECT_FALSE(
            parent.children[0]->content.isTerrainAvailabilityUpsample());
        EXPECT_TRUE(
            parent.children[1]->content.isTerrainAvailabilityUpsample());
        EXPECT_TRUE(
            parent.children[2]->content.isTerrainAvailabilityUpsample());
        EXPECT_TRUE(
            parent.children[3]->content.isTerrainAvailabilityUpsample());
    }
}

TEST(TileChildMaterializerTest,
     ExistingContentChildrenClearAvailabilityBoundaryRetryLikeCesiumNative) {
    auto scheme = TileScheme::createGeographicTMS();
    TilesetTile parent(
        TileKey{"Geographic-TMS", 0, 0, 0},
        scheme->tileToRectangle(TileKey{"Geographic-TMS", 0, 0, 0}));
    parent.content.loadState = TileLoadState::ContentLoading;

    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;
    int ensureCalls = 0;
    auto ensure = [&tiles, &scheme, &ensureCalls](
                      const TileKey& key) -> TilesetTile* {
        ++ensureCalls;
        const std::string cacheKey = cacheKeyFor(key);
        auto it = tiles.find(cacheKey);
        if (it == tiles.end()) {
            it = tiles
                     .emplace(
                         cacheKey,
                         std::make_unique<TilesetTile>(
                             key,
                             scheme->tileToRectangle(key)))
                     .first;
        }
        return it->second.get();
    };
    auto unavailable = [](const TileKey&) {
        return TileAvailabilityState::NotAvailable;
    };

    const TileChildFrameMaterializeResult result =
        TileChildFrameMaterializer::ensureChildren(
            TileChildFrameMaterializeInput{
                parent,
                {TileKey{"Geographic-TMS", 1, 0, 0}},
                2,
                true,
                true},
            ensure,
            unavailable);

    EXPECT_TRUE(result.changed);
    EXPECT_FALSE(result.retryLater);
    EXPECT_EQ(1, ensureCalls);
    ASSERT_EQ(1u, parent.children.size());
    EXPECT_EQ((TileKey{"Geographic-TMS", 1, 0, 0}),
              parent.children.front()->key);
    EXPECT_FALSE(parent.children.front()->content.derivesTerrainFromParent());
}

TEST(TileChildMaterializerTest,
     StableFrameMaterializationUsesStateVersionFastPath) {
    auto scheme = TileScheme::createGeographicTMS();
    const TileKey parentKey{"Geographic-TMS", 0, 0, 0};
    TilesetTile parent(parentKey, scheme->tileToRectangle(parentKey));
    parent.geometricError = 100.0;

    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;
    auto ensure = [&tiles, &scheme](const TileKey& key) -> TilesetTile* {
        const std::string cacheKey = cacheKeyFor(key);
        auto it = tiles.find(cacheKey);
        if (it == tiles.end()) {
            it = tiles.emplace(
                cacheKey,
                std::make_unique<TilesetTile>(
                    key,
                    scheme->tileToRectangle(key)))
                     .first;
        }
        return it->second.get();
    };
    int availabilityChecks = 0;
    auto availability = [&availabilityChecks](const TileKey&) {
        ++availabilityChecks;
        return TileAvailabilityState::Available;
    };
    TileChildFrameMaterializeInput input{
        parent,
        {},
        2,
        true,
        false,
        true,
        nullptr,
        7};

    const TileChildFrameMaterializeResult first =
        TileChildFrameMaterializer::ensureChildren(
            input,
            ensure,
            availability);
    const TileChildFrameMaterializeResult second =
        TileChildFrameMaterializer::ensureChildren(
            input,
            ensure,
            availability);

    EXPECT_TRUE(first.changed);
    EXPECT_FALSE(first.fastPath);
    EXPECT_FALSE(second.changed);
    EXPECT_TRUE(second.fastPath);
    EXPECT_EQ(4, availabilityChecks);
    EXPECT_EQ(4u, parent.children.size());
}

TEST(TileChildMaterializerTest,
     IncompleteMaterializationBacksOffUntilInputChanges) {
    auto scheme = TileScheme::createGeographicTMS();
    const TileKey parentKey{"Geographic-TMS", 0, 0, 0};
    TilesetTile parent(parentKey, scheme->tileToRectangle(parentKey));
    parent.geometricError = 100.0;

    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;
    bool childAvailable = false;
    int ensureCalls = 0;
    auto ensure = [&tiles, &scheme, &childAvailable, &ensureCalls](
                      const TileKey& key) -> TilesetTile* {
        ++ensureCalls;
        if (!childAvailable) {
            return nullptr;  // 子瓦片未就绪 → linkContentChildren 判不完成
        }
        const std::string cacheKey = cacheKeyFor(key);
        auto it = tiles.find(cacheKey);
        if (it == tiles.end()) {
            it = tiles.emplace(
                cacheKey,
                std::make_unique<TilesetTile>(
                    key, scheme->tileToRectangle(key)))
                     .first;
        }
        return it->second.get();
    };
    auto availability = [](const TileKey&) {
        return TileAvailabilityState::Available;
    };
    TileChildFrameMaterializeInput input{
        parent,
        {TileKey{"Geographic-TMS", 1, 0, 0}},
        2,
        true,
        false,
        true,
        nullptr,
        7};

    const TileChildFrameMaterializeResult first =
        TileChildFrameMaterializer::ensureChildren(
            input, ensure, availability);
    EXPECT_FALSE(first.fastPath);
    EXPECT_EQ(1, ensureCalls);
    EXPECT_FALSE(parent.childMaterializationStateValid);

    // 输入/拓扑 revision 未变 → 背压早退,不再每帧全量重走。
    const TileChildFrameMaterializeResult second =
        TileChildFrameMaterializer::ensureChildren(
            input, ensure, availability);
    EXPECT_FALSE(second.fastPath);
    EXPECT_FALSE(second.changed);
    EXPECT_EQ(1, ensureCalls) << "内容未变:背压应阻止每帧重走 ensureChildren";

    // 子瓦片就绪 + 输入 revision 变化(真实路径=子瓦片内容/包围体到达经
    // notifyChildMaterializationStateChanged bump)→ 恢复重试并完成物化。
    childAvailable = true;
    parent.invalidateChildMaterialization();
    const TileChildFrameMaterializeResult third =
        TileChildFrameMaterializer::ensureChildren(
            input, ensure, availability);
    EXPECT_TRUE(third.changed);
    EXPECT_EQ(2, ensureCalls);
    ASSERT_EQ(1u, parent.children.size());
    EXPECT_EQ((TileKey{"Geographic-TMS", 1, 0, 0}),
              parent.children.front()->key);
}

TEST(TileChildMaterializerTest,
     ChildTopologyRevisionInvalidatesStableMaterialization) {
    auto scheme = TileScheme::createGeographicTMS();
    const TileKey parentKey{"Geographic-TMS", 0, 0, 0};
    TilesetTile parent(parentKey, scheme->tileToRectangle(parentKey));
    parent.geometricError = 100.0;

    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;
    auto ensure = [&tiles, &scheme](const TileKey& key) -> TilesetTile* {
        const std::string cacheKey = cacheKeyFor(key);
        auto it = tiles.find(cacheKey);
        if (it == tiles.end()) {
            it = tiles.emplace(
                cacheKey,
                std::make_unique<TilesetTile>(
                    key,
                    scheme->tileToRectangle(key)))
                     .first;
        }
        return it->second.get();
    };
    bool available = false;
    int availabilityChecks = 0;
    auto availability = [&available, &availabilityChecks](const TileKey&) {
        ++availabilityChecks;
        return available
            ? TileAvailabilityState::Available
            : TileAvailabilityState::NotAvailable;
    };
    TileChildFrameMaterializeInput input{
        parent,
        {},
        2,
        true,
        false,
        true,
        nullptr,
        1};

    EXPECT_FALSE(
        TileChildFrameMaterializer::ensureChildren(
            input,
            ensure,
            availability)
            .changed);
    EXPECT_TRUE(
        TileChildFrameMaterializer::ensureChildren(
            input,
            ensure,
            availability)
            .fastPath);
    EXPECT_EQ(4, availabilityChecks);

    available = true;
    input.childTopologyRevision = 2;
    const TileChildFrameMaterializeResult refreshed =
        TileChildFrameMaterializer::ensureChildren(
            input,
            ensure,
            availability);

    EXPECT_TRUE(refreshed.changed);
    EXPECT_FALSE(refreshed.fastPath);
    EXPECT_EQ(8, availabilityChecks);
    EXPECT_EQ(4u, parent.children.size());
}

TEST(TileChildMaterializerTest,
     RetryLaterDoesNotPopulateMaterializationFastPath) {
    TilesetTile parent(
        TileKey{"Geographic-TMS", 0, 0, 0},
        Rectangle{});
    int availabilityChecks = 0;
    auto availability = [&availabilityChecks](const TileKey&) {
        ++availabilityChecks;
        return TileAvailabilityState::Available;
    };
    auto ensure = [](const TileKey&) -> TilesetTile* {
        return nullptr;
    };
    TileChildFrameMaterializeInput input{
        parent,
        {},
        2,
        true,
        true,
        true,
        nullptr,
        1};

    const TileChildFrameMaterializeResult first =
        TileChildFrameMaterializer::ensureChildren(
            input,
            ensure,
            availability);
    const TileChildFrameMaterializeResult second =
        TileChildFrameMaterializer::ensureChildren(
            input,
            ensure,
            availability);

    EXPECT_TRUE(first.retryLater);
    EXPECT_TRUE(second.retryLater);
    EXPECT_FALSE(first.fastPath);
    EXPECT_FALSE(second.fastPath);
    EXPECT_FALSE(parent.childMaterializationStateValid);
    EXPECT_EQ(0, availabilityChecks);
}

TEST(TileChildMaterializerTest,
     PartialChildMaterializationDoesNotPopulateFastPath) {
    auto scheme = TileScheme::createGeographicTMS();
    const TileKey parentKey{"Geographic-TMS", 0, 0, 0};
    TilesetTile parent(parentKey, scheme->tileToRectangle(parentKey));
    parent.geometricError = 100.0;

    bool allowEnsure = false;
    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;
    auto ensure =
        [&allowEnsure, &tiles, &scheme](const TileKey& key) -> TilesetTile* {
        if (!allowEnsure) {
            return nullptr;
        }
        const std::string cacheKey = cacheKeyFor(key);
        auto it = tiles.find(cacheKey);
        if (it == tiles.end()) {
            it = tiles.emplace(
                cacheKey,
                std::make_unique<TilesetTile>(
                    key,
                    scheme->tileToRectangle(key)))
                     .first;
        }
        return it->second.get();
    };
    int availabilityChecks = 0;
    auto availability = [&availabilityChecks](const TileKey&) {
        ++availabilityChecks;
        return TileAvailabilityState::Available;
    };
    TileChildFrameMaterializeInput input{
        parent,
        {},
        2,
        true,
        false,
        true,
        nullptr,
        1};

    const TileChildFrameMaterializeResult first =
        TileChildFrameMaterializer::ensureChildren(
            input,
            ensure,
            availability);

    EXPECT_FALSE(first.changed);
    EXPECT_FALSE(first.fastPath);
    EXPECT_FALSE(parent.childMaterializationStateValid);
    EXPECT_TRUE(parent.children.empty());
    EXPECT_EQ(4, availabilityChecks);

    allowEnsure = true;
    // 子瓦片变可创建 = 输入变化:真实路径里 ensureTile 建子瓦片/内容到达会经
    // notifyChildMaterializationStateChanged bump 父瓦片输入 revision,背压据此
    // 恢复重试(见 IncompleteMaterializationBacksOffUntilInputChanges)。
    parent.invalidateChildMaterialization();
    const TileChildFrameMaterializeResult second =
        TileChildFrameMaterializer::ensureChildren(
            input,
            ensure,
            availability);

    EXPECT_TRUE(second.changed);
    EXPECT_FALSE(second.fastPath);
    EXPECT_TRUE(parent.childMaterializationStateValid);
    EXPECT_EQ(4u, parent.children.size());
    EXPECT_EQ(8, availabilityChecks);
}

TEST(TileChildMaterializerTest,
     ChildStateMutationInvalidatesMaterializationFastPath) {
    auto scheme = TileScheme::createGeographicTMS();
    const TileKey parentKey{"Geographic-TMS", 0, 0, 0};
    TilesetTile parent(parentKey, scheme->tileToRectangle(parentKey));
    parent.geometricError = 100.0;

    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;
    auto ensure = [&tiles, &scheme](const TileKey& key) -> TilesetTile* {
        const std::string cacheKey = cacheKeyFor(key);
        auto it = tiles.find(cacheKey);
        if (it == tiles.end()) {
            it = tiles.emplace(
                cacheKey,
                std::make_unique<TilesetTile>(
                    key,
                    scheme->tileToRectangle(key)))
                     .first;
        }
        return it->second.get();
    };
    int availabilityChecks = 0;
    auto availability = [&availabilityChecks](const TileKey&) {
        ++availabilityChecks;
        return TileAvailabilityState::Available;
    };
    TileChildFrameMaterializeInput input{
        parent,
        {},
        2,
        true,
        false,
        true,
        nullptr,
        1};

    ASSERT_TRUE(
        TileChildFrameMaterializer::ensureChildren(
            input,
            ensure,
            availability)
            .changed);
    ASSERT_EQ(4u, parent.children.size());
    parent.children.front()->setGeometricError(999.0);

    const TileChildFrameMaterializeResult refreshed =
        TileChildFrameMaterializer::ensureChildren(
            input,
            ensure,
            availability);

    EXPECT_TRUE(refreshed.changed);
    EXPECT_FALSE(refreshed.fastPath);
    EXPECT_DOUBLE_EQ(50.0, parent.children.front()->geometricError);
    EXPECT_EQ(8, availabilityChecks);
}

TEST(TileChildMaterializerTest,
     AvailabilityBoundaryRetryReportsLatentUpsampleBookkeepingLikeCesiumNative) {
    TilesetTile parent(
        TileKey{"Geographic-TMS", 2, 0, 0},
        Rectangle{});
    parent.content.loadState = TileLoadState::ContentLoading;

    const std::array<TileKey, 4> childKeys{
        TileKey{"Geographic-TMS", 3, 0, 0},
        TileKey{"Geographic-TMS", 3, 1, 0},
        TileKey{"Geographic-TMS", 3, 0, 1},
        TileKey{"Geographic-TMS", 3, 1, 1}};
    auto availability = [&childKeys](const TileKey& key) {
        return key == childKeys[0]
            ? TileAvailabilityState::Available
            : TileAvailabilityState::NotAvailable;
    };

    int ensureCalls = 0;
    const TileChildFrameMaterializeResult result =
        TileChildFrameMaterializer::ensureChildren(
            TileChildFrameMaterializeInput{
                parent,
                {},
                8,
                true,
                true,
                true},
            [&ensureCalls](const TileKey&) -> TilesetTile* {
                ++ensureCalls;
                return nullptr;
            },
            availability);

    EXPECT_FALSE(result.changed);
    EXPECT_TRUE(result.retryLater);
    EXPECT_EQ(0, ensureCalls);
    EXPECT_TRUE(parent.children.empty());
}

TEST(TileChildMaterializerTest,
     AvailabilityBoundaryResolvedStatesMatchCesiumNative) {
    for (TileLoadState waitingState : {
             TileLoadState::Unloading,
             TileLoadState::FailedTemporarily,
             TileLoadState::Unloaded,
             TileLoadState::ContentLoading}) {
        EXPECT_FALSE(
            TileLoadStatePredicates::
                hasResolvedAvailabilityBoundaryContent(waitingState));
    }

    for (TileLoadState resolvedState : {
             TileLoadState::ContentLoaded,
             TileLoadState::Done,
             TileLoadState::Failed}) {
        EXPECT_TRUE(
            TileLoadStatePredicates::
                hasResolvedAvailabilityBoundaryContent(resolvedState));
    }
}

TEST(TileChildMaterializerTest,
     RetryLaterTerrainContentIsProtectedFromResidueCleanupLikeCesiumNative) {
    TilesetTile tile(
        TileKey{"Geographic-TMS", 0, 0, 0},
        Rectangle::fromDegrees(-180.0, -90.0, 0.0, 90.0));
    auto model = std::make_unique<GltfModel>();
    model->rasterOverlayDetails.setGeographicRectangle(tile.bounds);
    tile.content.contentKind = TileContentKind::Unknown;
    tile.content.loadState = TileLoadState::FailedTemporarily;
    tile.content.renderContent.setGltfContent(std::move(model));
    tile.content.renderContent.setTerrainRenderContent(true);
    tile.content.renderContent.addGltfPrimitiveResource(
        GltfPrimitiveRenderResources{});

    tile.content.loadState = TileLoadState::ContentLoading;
    EXPECT_TRUE(
        TileContentTerrainResiduePolicy::
            hasProtectedRetryableTerrainContent(tile));
    EXPECT_FALSE(TileContentTerrainResiduePolicy::hasRejectableResidue(tile));
    EXPECT_FALSE(TileContentTerrainResiduePolicy::clearRejectableResidue(tile));

    tile.content.loadState = TileLoadState::FailedTemporarily;
    tile.content.contentKind = TileContentKind::Unknown;
    EXPECT_FALSE(
        TileContentTerrainResiduePolicy::hasAcceptedTerrainContent(tile));
    EXPECT_TRUE(
        TileContentTerrainResiduePolicy::
            hasProtectedRetryableTerrainContent(tile));
    EXPECT_FALSE(TileContentTerrainResiduePolicy::hasRejectableResidue(tile));
    EXPECT_FALSE(TileContentTerrainResiduePolicy::clearRejectableResidue(tile));
    EXPECT_TRUE(tile.content.renderContent.hasGltfContent());
    EXPECT_TRUE(tile.content.renderContent.isTerrainRenderContent());
    EXPECT_TRUE(tile.content.renderContent.hasGltfPrimitiveResources());
}

TEST(TileChildMaterializerTest, NoAvailableTerrainChildrenCreatesNoneLikeCesiumNative) {
    TilesetTile parent(
        TileKey{"Geographic-TMS", 0, 0, 0},
        Rectangle{});

    int ensureCalls = 0;
    const bool changed = TileChildMaterializer::materializeTerrainChildren(
        parent,
        2,
        [](const TileKey&) {
            return TileAvailabilityState::NotAvailable;
        },
        [&ensureCalls](const TileKey&) -> TilesetTile* {
            ++ensureCalls;
            return nullptr;
        });

    EXPECT_FALSE(changed);
    EXPECT_EQ(0, ensureCalls);
    EXPECT_TRUE(parent.children.empty());
}

TEST(TileChildMaterializerTest, UnknownTerrainChildrenDoNotCreateUpsampledQuadLikeCesiumNative) {
    TilesetTile parent(
        TileKey{"Geographic-TMS", 0, 0, 0},
        Rectangle{});

    int ensureCalls = 0;
    const bool changed = TileChildMaterializer::materializeTerrainChildren(
        parent,
        2,
        [](const TileKey&) {
            return TileAvailabilityState::Unknown;
        },
        [&ensureCalls](const TileKey&) -> TilesetTile* {
            ++ensureCalls;
            return nullptr;
        });

    EXPECT_FALSE(changed);
    EXPECT_EQ(0, ensureCalls);
    EXPECT_TRUE(parent.children.empty());
}

TEST(TileChildMaterializerTest,
     TerrainAvailabilityUpsampledChildProbeMatchesCesiumBeforeChildrenExist) {
    TilesetTile parent(
        TileKey{"Geographic-TMS", 1, 1, 0},
        Rectangle{});

    auto availableCount = [](int count) {
        return [count, seen = 0](const TileKey&) mutable {
            return seen++ < count ? TileAvailabilityState::Available
                                  : TileAvailabilityState::NotAvailable;
        };
    };

    EXPECT_FALSE(TileChildMaterializer::hasTerrainAvailabilityUpsampledChild(
        parent,
        availableCount(0)));
    EXPECT_TRUE(TileChildMaterializer::hasTerrainAvailabilityUpsampledChild(
        parent,
        availableCount(1)));
    EXPECT_TRUE(TileChildMaterializer::hasTerrainAvailabilityUpsampledChild(
        parent,
        availableCount(3)));
    EXPECT_FALSE(TileChildMaterializer::hasTerrainAvailabilityUpsampledChild(
        parent,
        availableCount(4)));
}

TEST(TileChildMaterializerTest,
     TerrainAvailabilityUpsampledChildProbeMatchesCesiumAfterChildrenExist) {
    TilesetTile parent(
        TileKey{"Geographic-TMS", 1, 1, 0},
        Rectangle{});
    TilesetTile sw(TileKey{"Geographic-TMS", 2, 2, 0}, Rectangle{});
    TilesetTile se(TileKey{"Geographic-TMS", 2, 3, 0}, Rectangle{});
    TilesetTile nw(TileKey{"Geographic-TMS", 2, 2, 1}, Rectangle{});
    TilesetTile ne(TileKey{"Geographic-TMS", 2, 3, 1}, Rectangle{});
    parent.children = {&sw, &se, &nw, &ne};

    int availabilityChecks = 0;
    EXPECT_FALSE(TileChildMaterializer::hasTerrainAvailabilityUpsampledChild(
        parent,
        [&availabilityChecks](const TileKey&) {
            ++availabilityChecks;
            return TileAvailabilityState::Available;
        }));
    EXPECT_EQ(0, availabilityChecks);

    nw.content.markTerrainAvailabilityUpsample();
    EXPECT_TRUE(TileChildMaterializer::hasTerrainAvailabilityUpsampledChild(
        parent,
        [](const TileKey&) {
            return TileAvailabilityState::NotAvailable;
        }));
}

TEST(TileChildMaterializerTest,
     TerrainAvailabilityUpsampledTileDoesNotMaterializeChildrenLikeCesiumNative) {
    TilesetTile parent(
        TileKey{"Geographic-TMS", 1, 1, 0},
        Rectangle{});
    parent.content.markTerrainAvailabilityUpsample();

    int availabilityChecks = 0;
    int ensureCalls = 0;
    const bool changed = TileChildMaterializer::materializeTerrainChildren(
        parent,
        4,
        [&availabilityChecks](const TileKey&) {
            ++availabilityChecks;
            return TileAvailabilityState::Available;
        },
        [&ensureCalls](const TileKey&) -> TilesetTile* {
            ++ensureCalls;
            return nullptr;
        });

    EXPECT_FALSE(changed);
    EXPECT_EQ(0, availabilityChecks);
    EXPECT_EQ(0, ensureCalls);
    EXPECT_TRUE(parent.children.empty());
}

TEST(TileChildMaterializerTest,
     TerrainAvailabilityUpsampledTileFrameEnsureDoesNotCreateChildrenLikeCesiumNative) {
    TilesetTile parent(
        TileKey{"Geographic-TMS", 1, 1, 0},
        Rectangle{});
    parent.content.markTerrainAvailabilityUpsample();

    int availabilityChecks = 0;
    int ensureCalls = 0;
    TileChildFrameMaterializer::ensureChildren(
        TileChildFrameMaterializeInput{
            parent,
            {},
            4,
            true,
            false},
        [&ensureCalls](const TileKey&) -> TilesetTile* {
            ++ensureCalls;
            return nullptr;
        },
        [&availabilityChecks](const TileKey&) {
            ++availabilityChecks;
            return TileAvailabilityState::Available;
        });

    EXPECT_EQ(0, availabilityChecks);
    EXPECT_EQ(0, ensureCalls);
    EXPECT_TRUE(parent.children.empty());
}

TEST(TileChildMaterializerTest, MaterializeTerrainChildrenSkipsOutOfRangeGeographicTmsChildren) {
    TilesetTile parent(
        TileKey{"Geographic-TMS", 0, 2, 0},
        Rectangle{});
    int availabilityChecks = 0;
    int ensureCalls = 0;

    const bool changed = TileChildMaterializer::materializeTerrainChildren(
        parent,
        2,
        [&availabilityChecks](const TileKey&) {
            ++availabilityChecks;
            return TileAvailabilityState::Available;
        },
        [&ensureCalls](const TileKey&) -> TilesetTile* {
            ++ensureCalls;
            return nullptr;
        });

    EXPECT_FALSE(changed);
    EXPECT_EQ(0, availabilityChecks);
    EXPECT_EQ(0, ensureCalls);
    EXPECT_TRUE(parent.children.empty());
}

TEST(TileChildMaterializerTest,
     MaterializeTerrainChildrenKeepsRepresentableDeepGeographicTmsChildren) {
    TilesetTile parent(
        TileKey{"Geographic-TMS", 30, 1073741823, 536870911},
        Rectangle{});
    parent.geometricError = 64.0;
    parent.boundingVolume = TileBoundingVolume::fromRegion(
        parent.bounds,
        -10.0,
        250.0);

    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;
    auto ensure = [&tiles](const TileKey& key) -> TilesetTile* {
        const std::string keyString = cacheKeyFor(key);
        auto it = tiles.find(keyString);
        if (it == tiles.end()) {
            it = tiles
                     .emplace(
                         keyString,
                         std::make_unique<TilesetTile>(key, Rectangle{}))
                     .first;
        }
        return it->second.get();
    };

    int availabilityChecks = 0;
    const bool changed = TileChildMaterializer::materializeTerrainChildren(
        parent,
        31,
        [&availabilityChecks](const TileKey& key) {
            ++availabilityChecks;
            return key == TileKey{"Geographic-TMS", 31, 2147483647, 1073741823}
                ? TileAvailabilityState::Available
                : TileAvailabilityState::NotAvailable;
        },
        ensure);

    ASSERT_TRUE(changed);
    EXPECT_EQ(4, availabilityChecks);
    ASSERT_EQ(4u, parent.children.size());
    EXPECT_EQ((TileKey{"Geographic-TMS", 31, 2147483646, 1073741822}),
              parent.children[0]->key);
    EXPECT_EQ((TileKey{"Geographic-TMS", 31, 2147483647, 1073741822}),
              parent.children[1]->key);
    EXPECT_EQ((TileKey{"Geographic-TMS", 31, 2147483646, 1073741823}),
              parent.children[2]->key);
    EXPECT_EQ((TileKey{"Geographic-TMS", 31, 2147483647, 1073741823}),
              parent.children[3]->key);
}

TEST(TileChildMaterializerTest, NonRootUnavailableTerrainSiblingsBecomeUpsampledLikeCesiumNative) {
    TilesetTile parent(
        TileKey{"Geographic-TMS", 1, 1, 0},
        Rectangle{});

    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;
    auto ensure = [&tiles](const TileKey& key) -> TilesetTile* {
        const std::string cacheKey = cacheKeyFor(key);
        auto it = tiles.find(cacheKey);
        if (it == tiles.end()) {
            it = tiles.emplace(
                cacheKey,
                std::make_unique<TilesetTile>(key, Rectangle{})).first;
        }
        return it->second.get();
    };

    const bool changed = TileChildMaterializer::materializeTerrainChildren(
        parent,
        3,
        [](const TileKey& key) {
            return key.x == 2 && key.y == 0
                ? TileAvailabilityState::Available
                : TileAvailabilityState::NotAvailable;
        },
        ensure,
        true);

    ASSERT_TRUE(changed);
    ASSERT_EQ(4u, parent.children.size());

    EXPECT_EQ((TileKey{"Geographic-TMS", 2, 2, 0}), parent.children[0]->key);
    EXPECT_EQ((TileKey{"Geographic-TMS", 2, 3, 0}), parent.children[1]->key);
    EXPECT_EQ((TileKey{"Geographic-TMS", 2, 2, 1}), parent.children[2]->key);
    EXPECT_EQ((TileKey{"Geographic-TMS", 2, 3, 1}), parent.children[3]->key);

    EXPECT_FALSE(parent.children[0]->content.isTerrainAvailabilityUpsample());
    EXPECT_TRUE(parent.children[1]->content.isTerrainAvailabilityUpsample());
    EXPECT_TRUE(parent.children[2]->content.isTerrainAvailabilityUpsample());
    EXPECT_TRUE(parent.children[3]->content.isTerrainAvailabilityUpsample());
}

TEST(TileChildMaterializerTest,
     UnknownTerrainSiblingsBecomeUpsampledWhenAnyChildIsAvailableLikeCesiumNative) {
    TilesetTile parent(
        TileKey{"Geographic-TMS", 1, 1, 0},
        Rectangle{});

    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;
    auto ensure = [&tiles](const TileKey& key) -> TilesetTile* {
        const std::string cacheKey = cacheKeyFor(key);
        auto it = tiles.find(cacheKey);
        if (it == tiles.end()) {
            it = tiles.emplace(
                cacheKey,
                std::make_unique<TilesetTile>(key, Rectangle{})).first;
        }
        return it->second.get();
    };

    const bool changed = TileChildMaterializer::materializeTerrainChildren(
        parent,
        3,
        [](const TileKey& key) {
            if (key == TileKey{"Geographic-TMS", 2, 2, 0}) {
                return TileAvailabilityState::Available;
            }
            return TileAvailabilityState::Unknown;
        },
        ensure,
        true);

    ASSERT_TRUE(changed);
    ASSERT_EQ(4u, parent.children.size());

    EXPECT_FALSE(parent.children[0]->content.isTerrainAvailabilityUpsample());
    EXPECT_TRUE(parent.children[1]->content.isTerrainAvailabilityUpsample());
    EXPECT_TRUE(parent.children[2]->content.isTerrainAvailabilityUpsample());
    EXPECT_TRUE(parent.children[3]->content.isTerrainAvailabilityUpsample());
}

TEST(TileChildMaterializerTest,
     TerrainAvailabilityMaterializationClearsStaleUnconditionalRefine) {
    TilesetTile parent(
        TileKey{"Geographic-TMS", 1, 1, 0},
        Rectangle{});

    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;
    auto ensure = [&tiles](const TileKey& key) -> TilesetTile* {
        const std::string cacheKey = cacheKeyFor(key);
        auto it = tiles.find(cacheKey);
        if (it == tiles.end()) {
            it = tiles.emplace(
                cacheKey,
                std::make_unique<TilesetTile>(key, Rectangle{})).first;
        }
        return it->second.get();
    };

    TilesetTile* staleChild = ensure(TileKey{"Geographic-TMS", 2, 3, 0});
    ASSERT_NE(nullptr, staleChild);
    staleChild->unconditionallyRefine = true;

    const bool changed = TileChildMaterializer::materializeTerrainChildren(
        parent,
        3,
        [](const TileKey& key) {
            return key.x == 2 && key.y == 0
                ? TileAvailabilityState::Available
                : TileAvailabilityState::NotAvailable;
        },
        ensure,
        true);

    EXPECT_TRUE(changed);
    ASSERT_EQ(4u, parent.children.size());
    EXPECT_FALSE(staleChild->unconditionallyRefine);
}

TEST(TileChildMaterializerTest,
     TerrainAvailabilityReportsExistingChildUnconditionalRefineCleanup) {
    auto scheme = TileScheme::createGeographicTMS();
    TilesetTile parent(
        TileKey{"Geographic-TMS", 1, 1, 0},
        scheme->tileToRectangle(TileKey{"Geographic-TMS", 1, 1, 0}));
    parent.geometricError = 80.0;
    parent.refine = TileRefine::Replace;
    parent.boundingVolume = TileBoundingVolume::fromRegion(
        parent.bounds,
        -12.0,
        34.0);
    parent.content.renderContent.setTerrainHeightRange(-12.0, 34.0);

    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;
    auto ensure = [&tiles, &scheme](const TileKey& key) -> TilesetTile* {
        const std::string cacheKey = cacheKeyFor(key);
        auto it = tiles.find(cacheKey);
        if (it == tiles.end()) {
            it = tiles.emplace(
                cacheKey,
                std::make_unique<TilesetTile>(
                    key,
                    scheme->tileToRectangle(key))).first;
        }
        return it->second.get();
    };

    const std::array<TileKey, 4> childKeys = {
        TileKey{"Geographic-TMS", 2, 2, 0},
        TileKey{"Geographic-TMS", 2, 3, 0},
        TileKey{"Geographic-TMS", 2, 2, 1},
        TileKey{"Geographic-TMS", 2, 3, 1}};
    for (size_t i = 0; i < childKeys.size(); ++i) {
        TilesetTile* child = ensure(childKeys[i]);
        ASSERT_NE(nullptr, child);
        child->geometricError = 40.0;
        child->refine = TileRefine::Replace;
        child->boundingVolume = TileBoundingVolume::fromLooseRegion(
            child->bounds,
            -12.0,
            34.0);
        child->content.renderContent.setTerrainHeightRange(-12.0, 34.0);
        child->contentBoundingVolume.reset();
        if (i != 0) {
            child->content.markTerrainAvailabilityUpsample();
        }
        parent.children.push_back(child);
        child->parent = &parent;
    }
    parent.children[2]->unconditionallyRefine = true;

    const bool changed = TileChildMaterializer::materializeTerrainChildren(
        parent,
        3,
        [](const TileKey& key) {
            return key.x == 2 && key.y == 0
                ? TileAvailabilityState::Available
                : TileAvailabilityState::NotAvailable;
        },
        ensure,
        true);

    EXPECT_TRUE(changed);
    ASSERT_EQ(4u, parent.children.size());
    EXPECT_FALSE(parent.children[2]->unconditionallyRefine);
    EXPECT_FALSE(parent.children[0]->content.isTerrainAvailabilityUpsample());
    EXPECT_TRUE(parent.children[1]->content.isTerrainAvailabilityUpsample());
    EXPECT_TRUE(parent.children[2]->content.isTerrainAvailabilityUpsample());
    EXPECT_TRUE(parent.children[3]->content.isTerrainAvailabilityUpsample());
}

TEST(TileChildMaterializerTest,
     TerrainAvailabilityMaterializationRefreshesPartialGltfResidue) {
    auto scheme = TileScheme::createGeographicTMS();
    TilesetTile parent(
        TileKey{"Geographic-TMS", 1, 1, 0},
        scheme->tileToRectangle(TileKey{"Geographic-TMS", 1, 1, 0}));
    parent.geometricError = 80.0;
    parent.refine = TileRefine::Add;
    parent.boundingVolume = TileBoundingVolume::fromRegion(
        parent.bounds,
        -12.0,
        34.0);
    parent.content.renderContent.setTerrainHeightRange(-12.0, 34.0);

    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;
    auto ensure = [&tiles, &scheme](const TileKey& key) -> TilesetTile* {
        const std::string cacheKey = cacheKeyFor(key);
        auto it = tiles.find(cacheKey);
        if (it == tiles.end()) {
            it = tiles.emplace(
                cacheKey,
                std::make_unique<TilesetTile>(
                    key,
                    scheme->tileToRectangle(key))).first;
        }
        return it->second.get();
    };

    TilesetTile* acceptedChild =
        ensure(TileKey{"Geographic-TMS", 2, 2, 0});
    ASSERT_NE(nullptr, acceptedChild);
    acceptedChild->geometricError = 999.0;
    acceptedChild->refine = TileRefine::Replace;
    acceptedChild->boundingVolume = TileBoundingVolume::fromRegion(
        acceptedChild->bounds,
        1.0,
        2.0);
    acceptedChild->contentBoundingVolume = TileBoundingVolume::fromRegion(
        acceptedChild->bounds,
        3.0,
        4.0);
    auto acceptedModel = std::make_unique<GltfModel>();
    acceptedModel->rasterOverlayDetails.setGeographicRectangle(
        acceptedChild->bounds,
        1.0,
        2.0);
    acceptedChild->content.renderContent.setGltfContent(
        std::move(acceptedModel));
    acceptedChild->content.renderContent.setTerrainRenderContent(true);
    ASSERT_FALSE(
        TileContentTerrainResiduePolicy::hasAcceptedTerrainContent(
            *acceptedChild));

    const bool changed = TileChildMaterializer::materializeTerrainChildren(
        parent,
        3,
        [](const TileKey& key) {
            return key.x == 2 && key.y == 0
                ? TileAvailabilityState::Available
                : TileAvailabilityState::NotAvailable;
        },
        ensure,
        true);

    EXPECT_TRUE(changed);
    ASSERT_EQ(4u, parent.children.size());
    EXPECT_EQ(acceptedChild, parent.children[0]);
    EXPECT_FALSE(acceptedChild->content.derivesTerrainFromParent());
    EXPECT_FALSE(acceptedChild->content.renderContent.hasGltfModel());
    EXPECT_FALSE(acceptedChild->content.renderContent.isTerrainRenderContent());
    EXPECT_DOUBLE_EQ(40.0, acceptedChild->geometricError);
    EXPECT_EQ(TileRefine::Add, acceptedChild->refine);
    ASSERT_TRUE(acceptedChild->boundingVolume.has_value());
    EXPECT_EQ(TileBoundingVolumeKind::Region,
              acceptedChild->boundingVolume->kind);
    EXPECT_EQ(acceptedChild->bounds, acceptedChild->boundingVolume->region);
    EXPECT_TRUE(acceptedChild->boundingVolume->looseFittingHeights);
    EXPECT_DOUBLE_EQ(-12.0,
                     acceptedChild->boundingVolume->minimumHeight);
    EXPECT_DOUBLE_EQ(34.0,
                     acceptedChild->boundingVolume->maximumHeight);
    EXPECT_FALSE(acceptedChild->contentBoundingVolume.has_value());
    ASSERT_TRUE(
        acceptedChild->content.renderContent.hasTerrainHeightRange());
    EXPECT_DOUBLE_EQ(
        -12.0,
        acceptedChild->content.renderContent.terrainMinimumHeight());
    EXPECT_DOUBLE_EQ(
        34.0,
        acceptedChild->content.renderContent.terrainMaximumHeight());
}

TEST(TileChildMaterializerTest,
     TerrainAvailabilityMaterializationKeepsAcceptedUnavailableChildReal) {
    auto scheme = TileScheme::createGeographicTMS();
    TilesetTile parent(
        TileKey{"Geographic-TMS", 1, 1, 0},
        scheme->tileToRectangle(TileKey{"Geographic-TMS", 1, 1, 0}));
    parent.geometricError = 80.0;
    parent.refine = TileRefine::Replace;
    parent.boundingVolume = TileBoundingVolume::fromRegion(
        parent.bounds,
        -12.0,
        34.0);
    parent.content.renderContent.setTerrainHeightRange(-12.0, 34.0);

    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;
    auto ensure = [&tiles, &scheme](const TileKey& key) -> TilesetTile* {
        const std::string cacheKey = cacheKeyFor(key);
        auto it = tiles.find(cacheKey);
        if (it == tiles.end()) {
            it = tiles.emplace(
                cacheKey,
                std::make_unique<TilesetTile>(
                    key,
                    scheme->tileToRectangle(key))).first;
        }
        return it->second.get();
    };

    TilesetTile* acceptedChild =
        ensure(TileKey{"Geographic-TMS", 2, 3, 0});
    ASSERT_NE(nullptr, acceptedChild);
    auto acceptedModel = std::make_unique<GltfModel>();
    acceptedModel->rasterOverlayDetails.setGeographicRectangle(
        acceptedChild->bounds,
        1.0,
        2.0);
    GltfModel* rawModel = acceptedModel.get();
    acceptedChild->content.renderContent.setGltfContent(
        std::move(acceptedModel));
    acceptedChild->content.renderContent.setTerrainRenderContent(true);
    acceptedChild->content.renderContent.setGltfResourcesReady(true);
    acceptedChild->markRenderContentDone();
    acceptedChild->content.markTerrainAvailabilityUpsample();
    ASSERT_TRUE(
        TileContentTerrainResiduePolicy::hasAcceptedTerrainContent(
            *acceptedChild));

    const bool changed = TileChildMaterializer::materializeTerrainChildren(
        parent,
        3,
        [](const TileKey& key) {
            return key.x == 2 && key.y == 0
                ? TileAvailabilityState::Available
                : TileAvailabilityState::NotAvailable;
        },
        ensure,
        true);

    EXPECT_TRUE(changed);
    ASSERT_EQ(4u, parent.children.size());
    EXPECT_EQ(acceptedChild, parent.children[1]);
    EXPECT_FALSE(acceptedChild->content.isTerrainAvailabilityUpsample());
    EXPECT_EQ(rawModel,
              acceptedChild->content.renderContent.gltfModelForRead());
    EXPECT_TRUE(acceptedChild->content.renderContent.isTerrainRenderContent());
    EXPECT_TRUE(
        acceptedChild->content.renderContent.hasRasterOverlayDetailsContent());
    EXPECT_TRUE(
        TileContentTerrainResiduePolicy::hasAcceptedTerrainContent(
            *acceptedChild));
}

TEST(TileChildMaterializerTest,
     TerrainAvailabilityMaterializationClearsIncompleteGltfTerrainResidue) {
    DebugImageryProvider imagery;
    auto scheme = TileScheme::createGeographicTMS();
    RasterOverlayTileProvider provider(imagery, *scheme, nullptr);
    TilesetTile parent(
        TileKey{"Geographic-TMS", 1, 1, 0},
        scheme->tileToRectangle(TileKey{"Geographic-TMS", 1, 1, 0}));
    parent.geometricError = 80.0;
    parent.refine = TileRefine::Replace;
    parent.boundingVolume = TileBoundingVolume::fromRegion(
        parent.bounds,
        -12.0,
        34.0);
    parent.content.renderContent.setTerrainHeightRange(-12.0, 34.0);

    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;
    auto ensure = [&tiles, &scheme](const TileKey& key) -> TilesetTile* {
        const std::string cacheKey = cacheKeyFor(key);
        auto it = tiles.find(cacheKey);
        if (it == tiles.end()) {
            it = tiles.emplace(
                cacheKey,
                std::make_unique<TilesetTile>(
                    key,
                    scheme->tileToRectangle(key))).first;
        }
        return it->second.get();
    };

    TilesetTile* staleChild =
        ensure(TileKey{"Geographic-TMS", 2, 2, 0});
    ASSERT_NE(nullptr, staleChild);
    staleChild->content.renderContent.prepareGltfContent(
        makeQuadTerrainGltfModel(staleChild->bounds), Mat4::identity());
    staleChild->content.renderContent.setTerrainRenderContent(true);
    staleChild->content.renderContent.addGltfPrimitiveResource(
        GltfPrimitiveRenderResources{});
    staleChild->content.renderContent.markRenderContentReady();
    RasterMappedToTilesetTile& mapped =
        staleChild->rasterOverlayState.ensureMapping(0);
    std::vector<RasterOverlayProjection> missingProjections;
    mapped.update(
        staleChild->key,
        staleChild->content.renderContent.rasterOverlayDetails(),
        256.0,
        256.0,
        provider,
        nullptr,
        missingProjections);
    ASSERT_NE(nullptr, mapped.getLoadingTile());
    mapped.getLoadingTile()->setTexture(
        std::make_unique<DummyTexture>(4, 4));
    RecordingPrepareRendererResources prep;
    mapped.update(
        staleChild->key,
        staleChild->content.renderContent.rasterOverlayDetails(),
        256.0,
        256.0,
        provider,
        &prep,
        missingProjections);
    ASSERT_EQ(RasterMappedToTilesetTile::State::Attached,
              mapped.getState());
    ASSERT_EQ(1, prep.attachCount);

    staleChild->content.renderContent.setGltfContent(
        std::make_unique<GltfModel>());
    staleChild->content.renderContent.setTerrainRenderContent(true);
    staleChild->content.renderContent.setGltfResourcesReady(true);
    staleChild->rasterOverlayState.ensureMapping(0);
    ASSERT_FALSE(
        TileContentTerrainResiduePolicy::hasAcceptedTerrainContent(
            *staleChild));
    ASSERT_TRUE(
        TileContentTerrainResiduePolicy::hasRejectableResidue(*staleChild));

    const bool changed = TileChildMaterializer::materializeTerrainChildren(
        parent,
        3,
        [](const TileKey& key) {
            return key.x == 2 && key.y == 0
                ? TileAvailabilityState::Available
                : TileAvailabilityState::NotAvailable;
        },
        ensure,
        true,
        &prep);

    EXPECT_TRUE(changed);
    ASSERT_EQ(4u, parent.children.size());
    EXPECT_EQ(staleChild, parent.children[0]);
    EXPECT_FALSE(staleChild->content.isTerrainAvailabilityUpsample());
    EXPECT_FALSE(staleChild->content.renderContent.hasGltfContent());
    EXPECT_FALSE(staleChild->content.renderContent.isRenderContentReady());
    EXPECT_EQ(0u, staleChild->rasterOverlayState.mappingCount());
    EXPECT_EQ(1, prep.detachCount);
    EXPECT_EQ(staleChild->key, prep.lastDetachedGeometryKey);
    EXPECT_EQ(0, prep.lastDetachedOverlayIndex);
}

TEST(TileChildMaterializerTest,
     TerrainAvailabilityUpgradeClearsStaleUpsampledMesh) {
    TilesetTile parent(
        TileKey{"Geographic-TMS", 1, 1, 0},
        Rectangle{});

    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;
    auto ensure = [&tiles](const TileKey& key) -> TilesetTile* {
        const std::string cacheKey = cacheKeyFor(key);
        auto it = tiles.find(cacheKey);
        if (it == tiles.end()) {
            it = tiles.emplace(
                cacheKey,
                std::make_unique<TilesetTile>(key, Rectangle{})).first;
        }
        return it->second.get();
    };

    ASSERT_TRUE(TileChildMaterializer::materializeTerrainChildren(
        parent,
        3,
        [](const TileKey& key) {
            return key.x == 2 && key.y == 0
                ? TileAvailabilityState::Available
                : TileAvailabilityState::NotAvailable;
        },
        ensure));
    ASSERT_EQ(4u, parent.children.size());
    TilesetTile* upgradedChild = parent.children[1];
    ASSERT_EQ((TileKey{"Geographic-TMS", 2, 3, 0}), upgradedChild->key);
    ASSERT_TRUE(upgradedChild->content.isTerrainAvailabilityUpsample());
    upgradedChild->content.renderContent.prepareGltfContent(
        makeQuadTerrainGltfModel(upgradedChild->bounds), Mat4::identity());
    upgradedChild->content.renderContent.setTerrainRenderContent(true);
    upgradedChild->content.renderContent.addGltfPrimitiveResource(
        GltfPrimitiveRenderResources{});
    upgradedChild->content.renderContent.markRenderContentReady();

    const bool changed = TileChildMaterializer::materializeTerrainChildren(
        parent,
        3,
        [](const TileKey& key) {
            return key.y == 0
                ? TileAvailabilityState::Available
                : TileAvailabilityState::NotAvailable;
        },
        ensure);

    EXPECT_TRUE(changed);
    EXPECT_FALSE(upgradedChild->content.derivesTerrainFromParent());
    EXPECT_FALSE(upgradedChild->content.renderContent.isMeshReady());
    EXPECT_FALSE(upgradedChild->content.renderContent.isSurfaceDrawable());
    EXPECT_EQ(4u, parent.children.size());
}

TEST(TileChildMaterializerTest,
     TerrainAvailabilityMaterializationReplacesRasterDetailUpsampleKind) {
    TilesetTile parent(
        TileKey{"Geographic-TMS", 1, 1, 0},
        Rectangle{});

    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;
    auto ensure = [&tiles](const TileKey& key) -> TilesetTile* {
        const std::string cacheKey = cacheKeyFor(key);
        auto it = tiles.find(cacheKey);
        if (it == tiles.end()) {
            it = tiles.emplace(
                cacheKey,
                std::make_unique<TilesetTile>(key, Rectangle{})).first;
        }
        return it->second.get();
    };

    TilesetTile* staleRasterChild =
        ensure(TileKey{"Geographic-TMS", 2, 3, 0});
    ASSERT_NE(nullptr, staleRasterChild);
    staleRasterChild->content.markRasterDetailUpsample(
        RasterOverlayProjection::WebMercator);
    staleRasterChild->content.renderContent.prepareGltfContent(
        makeQuadTerrainGltfModel(staleRasterChild->bounds), Mat4::identity());
    staleRasterChild->content.renderContent.setTerrainRenderContent(true);
    staleRasterChild->content.renderContent.addGltfPrimitiveResource(
        GltfPrimitiveRenderResources{});
    staleRasterChild->content.renderContent.markRenderContentReady();

    const bool changed = TileChildMaterializer::materializeTerrainChildren(
        parent,
        3,
        [](const TileKey& key) {
            return key.x == 2 && key.y == 0
                ? TileAvailabilityState::Available
                : TileAvailabilityState::NotAvailable;
        },
        ensure,
        true);

    EXPECT_TRUE(changed);
    ASSERT_EQ(4u, parent.children.size());
    EXPECT_TRUE(staleRasterChild->content.isTerrainAvailabilityUpsample());
    EXPECT_FALSE(staleRasterChild->content.isRasterDetailUpsample());
    EXPECT_FALSE(staleRasterChild->content.rasterDetailSourceProjection);
    EXPECT_FALSE(staleRasterChild->content.renderContent.isMeshReady());
}

TEST(TileChildMaterializerTest,
     TerrainAvailabilityMaterializationClearsStaleGltfRenderContent) {
    TilesetTile parent(
        TileKey{"Geographic-TMS", 1, 1, 0},
        Rectangle{});

    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;
    auto ensure = [&tiles](const TileKey& key) -> TilesetTile* {
        const std::string cacheKey = cacheKeyFor(key);
        auto it = tiles.find(cacheKey);
        if (it == tiles.end()) {
            it = tiles.emplace(
                cacheKey,
                std::make_unique<TilesetTile>(key, Rectangle{})).first;
        }
        return it->second.get();
    };

    TilesetTile* staleRasterChild =
        ensure(TileKey{"Geographic-TMS", 2, 3, 0});
    ASSERT_NE(nullptr, staleRasterChild);
    staleRasterChild->content.markRasterDetailUpsample();
    staleRasterChild->content.renderContent.setGltfContent(
        std::make_unique<GltfModel>());
    staleRasterChild->content.renderContent.addGltfPrimitiveResource(
        GltfPrimitiveRenderResources{});
    staleRasterChild->content.renderContent.markRenderContentReady();

    const bool changed = TileChildMaterializer::materializeTerrainChildren(
        parent,
        3,
        [](const TileKey& key) {
            return key.x == 2 && key.y == 0
                ? TileAvailabilityState::Available
                : TileAvailabilityState::NotAvailable;
        },
        ensure,
        true);

    EXPECT_TRUE(changed);
    ASSERT_EQ(4u, parent.children.size());
    EXPECT_TRUE(staleRasterChild->content.isTerrainAvailabilityUpsample());
    EXPECT_FALSE(staleRasterChild->content.isRasterDetailUpsample());
    EXPECT_FALSE(staleRasterChild->content.renderContent.hasGltfModel());
    EXPECT_FALSE(staleRasterChild->content.renderContent.hasGltfResources());
    EXPECT_FALSE(staleRasterChild->content.renderContent.isRenderContentReady());
}

TEST(TileChildMaterializerTest,
     TerrainAvailabilityUpsampleRemainsContentlessLikeCesiumNative) {
    TilesetTile parent(
        TileKey{"Geographic-TMS", 1, 1, 0},
        Rectangle{});
    parent.content.renderContent.setTerrainHeightRange(-12.0, 34.0);

    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;
    auto ensure = [&tiles](const TileKey& key) -> TilesetTile* {
        const std::string cacheKey = cacheKeyFor(key);
        auto it = tiles.find(cacheKey);
        if (it == tiles.end()) {
            it = tiles.emplace(
                cacheKey,
                std::make_unique<TilesetTile>(key, Rectangle{})).first;
        }
        return it->second.get();
    };

    TilesetTile* staleUpsampledChild =
        ensure(TileKey{"Geographic-TMS", 2, 3, 0});
    ASSERT_NE(nullptr, staleUpsampledChild);
    staleUpsampledChild->content.markTerrainAvailabilityUpsample();
    staleUpsampledChild->content.renderContent.prepareGltfContent(
        makeQuadTerrainGltfModel(staleUpsampledChild->bounds),
        Mat4::identity());
    staleUpsampledChild->content.renderContent.setTerrainRenderContent(true);
    staleUpsampledChild->content.renderContent.addGltfPrimitiveResource(
        GltfPrimitiveRenderResources{});
    staleUpsampledChild->content.renderContent.markRenderContentReady();
    staleUpsampledChild->rasterOverlayState.ensureMapping(0);
    staleUpsampledChild->rasterOverlayState.missingProjections().push_back(
        RasterOverlayProjection{});

    const bool changed = TileChildMaterializer::materializeTerrainChildren(
        parent,
        3,
        [](const TileKey& key) {
            return key.x == 2 && key.y == 0
                ? TileAvailabilityState::Available
                : TileAvailabilityState::NotAvailable;
        },
        ensure,
        true);

    EXPECT_TRUE(changed);
    ASSERT_EQ(4u, parent.children.size());
    EXPECT_EQ(staleUpsampledChild, parent.children[1]);
    EXPECT_TRUE(staleUpsampledChild->content.isTerrainAvailabilityUpsample());
    EXPECT_FALSE(
        staleUpsampledChild->content.renderContent.hasRetainedHeightmap());
    EXPECT_FALSE(staleUpsampledChild->content.renderContent.hasGltfModel());
    EXPECT_FALSE(
        staleUpsampledChild->content.renderContent.hasGltfResources());
    EXPECT_FALSE(
        staleUpsampledChild->content.renderContent.isRenderContentReady());
    EXPECT_EQ(0u, staleUpsampledChild->rasterOverlayState.mappingCount());
    EXPECT_FALSE(
        staleUpsampledChild->rasterOverlayState.hasMissingProjections());
    EXPECT_TRUE(
        staleUpsampledChild->content.renderContent.hasTerrainHeightRange());
    EXPECT_DOUBLE_EQ(
        -12.0,
        staleUpsampledChild->content.renderContent.terrainMinimumHeight());
    EXPECT_DOUBLE_EQ(
        34.0,
        staleUpsampledChild->content.renderContent.terrainMaximumHeight());
}

TEST(TileChildMaterializerTest,
     TerrainAvailabilityMaterializationRefreshesReadyGltfGeometryLikeCesiumNative) {
    TilesetTile parent(
        TileKey{"Geographic-TMS", 1, 1, 0},
        Rectangle{});
    parent.content.renderContent.setTerrainHeightRange(-100.0, 200.0);

    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;
    auto ensure = [&tiles](const TileKey& key) -> TilesetTile* {
        const std::string cacheKey = cacheKeyFor(key);
        auto it = tiles.find(cacheKey);
        if (it == tiles.end()) {
            it = tiles.emplace(
                cacheKey,
                std::make_unique<TilesetTile>(key, Rectangle{})).first;
        }
        return it->second.get();
    };

    TilesetTile* readyGltfChild =
        ensure(TileKey{"Geographic-TMS", 2, 2, 0});
    ASSERT_NE(nullptr, readyGltfChild);
    readyGltfChild->content.renderContent.setGltfContent(
        std::make_unique<GltfModel>());
    readyGltfChild->content.renderContent.setTerrainRenderContent(true);
    readyGltfChild->content.renderContent.addGltfPrimitiveResource(
        GltfPrimitiveRenderResources{});
    readyGltfChild->content.renderContent.markRenderContentReady();
    readyGltfChild->content.renderContent.setTerrainHeightRange(1.0, 2.0);
    readyGltfChild->boundingVolume =
        TileBoundingVolume::fromRegion(readyGltfChild->bounds, 1.0, 2.0);

    EXPECT_TRUE(TileChildMaterializer::materializeTerrainChildren(
        parent,
        3,
        [](const TileKey& key) {
            return key.x == 2 && key.y == 0
                ? TileAvailabilityState::Available
                : TileAvailabilityState::NotAvailable;
        },
        ensure));

    EXPECT_TRUE(readyGltfChild->content.renderContent.hasGltfModel());
    EXPECT_TRUE(readyGltfChild->content.renderContent.isRenderContentReady());
    EXPECT_DOUBLE_EQ(
        -100.0,
        readyGltfChild->content.renderContent.terrainMinimumHeight());
    EXPECT_DOUBLE_EQ(
        200.0,
        readyGltfChild->content.renderContent.terrainMaximumHeight());
    ASSERT_TRUE(readyGltfChild->boundingVolume.has_value());
    EXPECT_DOUBLE_EQ(-100.0, readyGltfChild->boundingVolume->minimumHeight);
    EXPECT_DOUBLE_EQ(200.0, readyGltfChild->boundingVolume->maximumHeight);
}

TEST(TileChildMaterializerTest,
     ContentTerrainSurfaceResidueInheritsParentHeightRange) {
    TilesetTile parent(
        TileKey{"Geographic-TMS", 1, 1, 0},
        Rectangle{});
    TilesetTile contentTerrainChild(
        TileKey{"Geographic-TMS", 2, 2, 0},
        Rectangle{},
        &parent);
    TilesetTile legacySurfaceChild(
        TileKey{"Geographic-TMS", 2, 3, 0},
        Rectangle{},
        &parent);
    parent.children = {&contentTerrainChild, &legacySurfaceChild};

    contentTerrainChild.content.renderContent.prepareGltfContent(
        makeQuadTerrainGltfModel(contentTerrainChild.bounds),
        Mat4::identity());
    contentTerrainChild.content.renderContent.setTerrainRenderContent(true);
    contentTerrainChild.content.renderContent.addGltfPrimitiveResource(
        GltfPrimitiveRenderResources{});
    contentTerrainChild.content.renderContent.markRenderContentReady();

    legacySurfaceChild.content.renderContent.prepareGltfContent(
        makeQuadTerrainGltfModel(legacySurfaceChild.bounds),
        Mat4::identity());
    legacySurfaceChild.content.renderContent.setTerrainRenderContent(true);
    legacySurfaceChild.content.renderContent.addGltfPrimitiveResource(
        GltfPrimitiveRenderResources{});
    legacySurfaceChild.content.renderContent.markRenderContentReady();

    TileTerrainHeightRangePolicy::setTerrainHeightRange(
        parent,
        -100.0,
        200.0);
    TileTerrainHeightRangePolicy::inheritHeightRangeForUnreadyChildren(parent);

    // GlTF content is "ready" so children don't inherit parent height range
    EXPECT_DOUBLE_EQ(
        0.0,
        contentTerrainChild.content.renderContent.terrainMinimumHeight());
    EXPECT_DOUBLE_EQ(
        0.0,
        contentTerrainChild.content.renderContent.terrainMaximumHeight());
    EXPECT_DOUBLE_EQ(
        0.0,
        legacySurfaceChild.content.renderContent.terrainMinimumHeight());
    EXPECT_DOUBLE_EQ(
        0.0,
        legacySurfaceChild.content.renderContent.terrainMaximumHeight());
}

TEST(TileChildMaterializerTest, NonRootGeographicTerrainChildrenPreserveBounds) {
    auto scheme = TileScheme::createGeographicTMS();
    TilesetTile parent(
        TileKey{"Geographic-TMS", 1, 0, 1},
        scheme->tileToRectangle(TileKey{"Geographic-TMS", 1, 0, 1}));

    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;
    auto ensure = [&tiles, &scheme](const TileKey& key) -> TilesetTile* {
        const std::string cacheKey = cacheKeyFor(key);
        auto it = tiles.find(cacheKey);
        if (it == tiles.end()) {
            it = tiles.emplace(
                cacheKey,
                std::make_unique<TilesetTile>(
                    key,
                    scheme->tileToRectangle(key)))
                     .first;
        }
        return it->second.get();
    };

    const bool changed = TileChildMaterializer::materializeTerrainChildren(
        parent,
        3,
        [](const TileKey& key) {
            return key.x == 0 && key.y == 2
                ? TileAvailabilityState::Available
                : TileAvailabilityState::NotAvailable;
        },
        ensure);

    ASSERT_TRUE(changed);
    ASSERT_EQ(4u, parent.children.size());

    const TilesetTile* sw = parent.children[0];
    const TilesetTile* se = parent.children[1];
    const TilesetTile* nw = parent.children[2];
    const TilesetTile* ne = parent.children[3];
    ASSERT_NE(nullptr, sw);
    ASSERT_NE(nullptr, se);
    ASSERT_NE(nullptr, nw);
    ASSERT_NE(nullptr, ne);
    EXPECT_EQ((TileKey{"Geographic-TMS", 2, 0, 2}), sw->key);
    EXPECT_EQ((TileKey{"Geographic-TMS", 2, 1, 2}), se->key);
    EXPECT_EQ((TileKey{"Geographic-TMS", 2, 0, 3}), nw->key);
    EXPECT_EQ((TileKey{"Geographic-TMS", 2, 1, 3}), ne->key);
    EXPECT_NEAR(-MathUtils::OnePi, sw->bounds.west(), 1e-9);
    EXPECT_NEAR(0.0, sw->bounds.south(), 1e-9);
    EXPECT_NEAR(-MathUtils::OnePi * 0.75, sw->bounds.east(), 1e-9);
    EXPECT_NEAR(MathUtils::PiOverTwo * 0.5, sw->bounds.north(), 1e-9);
    EXPECT_NEAR(-MathUtils::OnePi * 0.75, ne->bounds.west(), 1e-9);
    EXPECT_NEAR(MathUtils::PiOverTwo * 0.5, ne->bounds.south(), 1e-9);
    EXPECT_NEAR(-MathUtils::PiOverTwo, ne->bounds.east(), 1e-9);
    EXPECT_NEAR(MathUtils::PiOverTwo, ne->bounds.north(), 1e-9);
    EXPECT_FALSE(sw->content.derivesTerrainFromParent());
    EXPECT_TRUE(se->content.derivesTerrainFromParent());
    EXPECT_TRUE(nw->content.derivesTerrainFromParent());
    EXPECT_TRUE(ne->content.derivesTerrainFromParent());
}

TEST(TileChildMaterializerTest,
     WebMercatorTerrainChildrenPreserveCesiumNativeSouthFirstOrder) {
    auto scheme = TileScheme::createXYZWebMercator();
    const TileKey parentKey{"XYZ-WebMercator", 0, 0, 0};
    TilesetTile parent(parentKey, scheme->tileToRectangle(parentKey));

    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;
    auto ensure = [&tiles, &scheme](const TileKey& key) -> TilesetTile* {
        const std::string cacheKey = cacheKeyFor(key);
        auto it = tiles.find(cacheKey);
        if (it == tiles.end()) {
            it = tiles.emplace(
                cacheKey,
                std::make_unique<TilesetTile>(
                    key,
                    scheme->tileToRectangle(key)))
                     .first;
        }
        return it->second.get();
    };

    const bool changed = TileChildMaterializer::materializeTerrainChildren(
        parent,
        2,
        [](const TileKey& key) {
            return key.x == 0 && key.y == 1
                ? TileAvailabilityState::Available
                : TileAvailabilityState::NotAvailable;
        },
        ensure);

    ASSERT_TRUE(changed);
    ASSERT_EQ(4u, parent.children.size());

    const TilesetTile* sw = parent.children[0];
    const TilesetTile* se = parent.children[1];
    const TilesetTile* nw = parent.children[2];
    const TilesetTile* ne = parent.children[3];
    ASSERT_NE(nullptr, sw);
    ASSERT_NE(nullptr, se);
    ASSERT_NE(nullptr, nw);
    ASSERT_NE(nullptr, ne);
    EXPECT_EQ((TileKey{"XYZ-WebMercator", 1, 0, 1}), sw->key);
    EXPECT_EQ((TileKey{"XYZ-WebMercator", 1, 1, 1}), se->key);
    EXPECT_EQ((TileKey{"XYZ-WebMercator", 1, 0, 0}), nw->key);
    EXPECT_EQ((TileKey{"XYZ-WebMercator", 1, 1, 0}), ne->key);
    EXPECT_FALSE(sw->content.derivesTerrainFromParent());
    EXPECT_TRUE(se->content.derivesTerrainFromParent());
    EXPECT_TRUE(nw->content.derivesTerrainFromParent());
    EXPECT_TRUE(ne->content.derivesTerrainFromParent());
    EXPECT_LE(sw->bounds.north(), nw->bounds.south());
    EXPECT_LE(se->bounds.north(), ne->bounds.south());
}

TEST(TileChildMaterializerTest, RasterUpsampledChildrenKeepSchemeBoundsAndRemainStable) {
    auto scheme = TileScheme::createGeographicTMS();
    TilesetTile parent(
        TileKey{"Geographic-TMS", 0, 0, 0},
        Rectangle::fromDegrees(-20.0, -10.0, 0.0, 10.0));
    parent.geometricError = 100.0;
    parent.content.renderContent.setTerrainHeightRange(-5.0, 25.0);

    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;
    auto ensure = [&tiles, &scheme](const TileKey& key) -> TilesetTile* {
        const std::string cacheKey = cacheKeyFor(key);
        auto it = tiles.find(cacheKey);
        if (it == tiles.end()) {
            it = tiles.emplace(
                cacheKey,
                std::make_unique<TilesetTile>(
                    key,
                    scheme->tileToRectangle(key))).first;
        }
        return it->second.get();
    };

    const bool changed =
        TileChildMaterializer::materializeRasterUpsampledChildren(
            parent,
            200.0,
            ensure);
    const bool changedAgain =
        TileChildMaterializer::materializeRasterUpsampledChildren(
            parent,
            200.0,
            ensure);

    ASSERT_TRUE(changed);
    EXPECT_FALSE(changedAgain);
    ASSERT_EQ(4u, parent.children.size());
    EXPECT_EQ(TileRefine::Replace, parent.refine);

    // The registry/scheme rectangle assigned by ensureTile stays
    // authoritative; the materializer must not overwrite it with
    // content-derived subdivision quadrants.
    EXPECT_EQ(
        scheme->tileToRectangle(TileKey{"Geographic-TMS", 1, 0, 0}),
        parent.children[0]->bounds);
    EXPECT_EQ(
        scheme->tileToRectangle(TileKey{"Geographic-TMS", 1, 1, 1}),
        parent.children[3]->bounds);

    for (TilesetTile* child : parent.children) {
        ASSERT_NE(nullptr, child);
        EXPECT_EQ(&parent, child->parent);
        EXPECT_TRUE(child->content.derivesTerrainFromParent());
        EXPECT_TRUE(child->content.isRasterDetailUpsample());
        EXPECT_DOUBLE_EQ(50.0, child->geometricError);
        EXPECT_EQ(scheme->tileToRectangle(child->key), child->bounds);
        ASSERT_TRUE(child->boundingVolume.has_value());
        EXPECT_EQ(TileBoundingVolumeKind::Region, child->boundingVolume->kind);
        EXPECT_EQ(child->bounds, child->boundingVolume->region);
        EXPECT_TRUE(child->content.renderContent.hasTerrainHeightRange());
        EXPECT_DOUBLE_EQ(
            -5.0,
            child->content.renderContent.terrainMinimumHeight());
        EXPECT_DOUBLE_EQ(
            25.0,
            child->content.renderContent.terrainMaximumHeight());
    }
}

TEST(TileRasterUpsampledChildMaterializerTest,
     GcjImageryUsesStableWorldProjectionForGeometrySubdivision) {
    RasterOverlayDetails geographicDetails;
    geographicDetails.rasterOverlayProjections = {
        RasterOverlayProjection::Gcj02WebMercator,
        RasterOverlayProjection::Geographic};
    geographicDetails.rasterOverlayRectangles = {
        Rectangle(1.0, 2.0, 3.0, 4.0),
        Rectangle::fromDegrees(100.0, 20.0, 110.0, 30.0)};
    EXPECT_EQ(
        RasterOverlayProjection::Geographic,
        TileRasterUpsampledChildMaterializer::
            geometryProjectionForRasterDetail(
                geographicDetails,
                RasterOverlayProjection::Gcj02WebMercator));

    RasterOverlayDetails webMercatorDetails;
    webMercatorDetails.rasterOverlayProjections = {
        RasterOverlayProjection::Gcj02WebMercator,
        RasterOverlayProjection::WebMercator};
    webMercatorDetails.rasterOverlayRectangles = {
        Rectangle(1.0, 2.0, 3.0, 4.0),
        Rectangle(5.0, 6.0, 7.0, 8.0)};
    EXPECT_EQ(
        RasterOverlayProjection::WebMercator,
        TileRasterUpsampledChildMaterializer::
            geometryProjectionForRasterDetail(
                webMercatorDetails,
                RasterOverlayProjection::Gcj02WebMercator));

    RasterOverlayDetails gcjOnlyDetails;
    gcjOnlyDetails.rasterOverlayProjections = {
        RasterOverlayProjection::Gcj02WebMercator};
    gcjOnlyDetails.rasterOverlayRectangles = {
        Rectangle(1.0, 2.0, 3.0, 4.0)};
    EXPECT_FALSE(
        TileRasterUpsampledChildMaterializer::
            geometryProjectionForRasterDetail(
                gcjOnlyDetails,
                RasterOverlayProjection::Gcj02WebMercator));
}

TEST(TileRasterUpsampledChildMaterializerTest,
     GcjMoreDetailMaterializesChildrenInStableWorldProjection) {
    DebugImageryProvider imagery;
    auto overlayScheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(
        imagery,
        *overlayScheme,
        nullptr,
        RasterOverlayGeoreference::Gcj02WebMercator);
    auto terrainScheme = TileScheme::createGeographicTMS();
    TilesetTile parent(
        TileKey{terrainScheme->id(), 0, 0, 0},
        Rectangle::fromDegrees(106.30, 29.30, 106.80, 29.80));
    parent.geometricError = 100.0;
    parent.content.renderContent.prepareGltfContent(
        makeQuadTerrainGltfModel(parent.bounds),
        Mat4::identity());
    parent.content.renderContent.setTerrainRenderContent(true);
    parent.content.renderContent.addGltfPrimitiveResource(
        GltfPrimitiveRenderResources{});
    parent.content.renderContent.markRenderContentReady();
    parent.content.renderContent.setTerrainHeightRange(-20.0, 120.0);

    RasterOverlayDetails* details =
        parent.content.renderContent.mutableRasterOverlayDetails();
    details->rasterOverlayProjections = {
        RasterOverlayProjection::Gcj02WebMercator,
        RasterOverlayProjection::Geographic};
    details->rasterOverlayRectangles = {
        projectWorldRectangleForRasterOverlay(
            parent.bounds,
            RasterOverlayProjection::Gcj02WebMercator),
        parent.bounds};
    details->boundingRegion = {parent.bounds, -20.0, 120.0};

    RasterMappedToTilesetTile& mapped =
        parent.rasterOverlayState.ensureMapping(0);
    std::vector<RasterOverlayProjection> missingProjections;
    mapped.update(
        parent.key,
        parent.content.renderContent.rasterOverlayDetails(),
        256.0,
        256.0,
        provider,
        nullptr,
        missingProjections);
    RasterOverlayTile* loadingTile = mapped.getLoadingTile();
    ASSERT_NE(nullptr, loadingTile);
    loadingTile->setState(RasterOverlayTile::LoadState::Loaded);
    loadingTile->setMoreDetailAvailable(
        RasterOverlayTile::MoreDetailAvailable::Yes);
    EXPECT_EQ(
        RasterMappedToTilesetTile::MoreDetail::Yes,
        mapped.update(
            parent.key,
            parent.content.renderContent.rasterOverlayDetails(),
            256.0,
            256.0,
            provider,
            nullptr,
            missingProjections));

    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;
    auto ensure =
        [&tiles, &terrainScheme](const TileKey& key) -> TilesetTile* {
        const std::string cacheKey = cacheKeyFor(key);
        auto it = tiles.find(cacheKey);
        if (it == tiles.end()) {
            it = tiles.emplace(
                cacheKey,
                std::make_unique<TilesetTile>(
                    key,
                    terrainScheme->tileToRectangle(key)))
                     .first;
        }
        return it->second.get();
    };

    ASSERT_TRUE(TileRasterUpsampledChildMaterializer::materialize(
        parent,
        100.0,
        ensure));
    ASSERT_EQ(4u, parent.children.size());
    for (TilesetTile* child : parent.children) {
        ASSERT_NE(nullptr, child);
        EXPECT_EQ(terrainScheme->tileToRectangle(child->key), child->bounds);
        EXPECT_TRUE(child->content.isRasterDetailUpsample());
        EXPECT_TRUE(child->content.derivesTerrainFromParent());
    }
}

TEST(TileChildMaterializerTest,
     RasterUpsampledChildrenRefreshWhenGeometricErrorChanges) {
    DebugImageryProvider imagery;
    auto scheme = TileScheme::createGeographicTMS();
    RasterOverlayTileProvider provider(imagery, *scheme, nullptr);
    TilesetTile parent(
        TileKey{"Geographic-TMS", 0, 0, 0},
        Rectangle::fromDegrees(-20.0, -10.0, 0.0, 10.0));
    parent.geometricError = 100.0;
    parent.content.renderContent.setTerrainHeightRange(-5.0, 25.0);

    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;
    auto ensure = [&tiles, &scheme](const TileKey& key) -> TilesetTile* {
        const std::string cacheKey = cacheKeyFor(key);
        auto it = tiles.find(cacheKey);
        if (it == tiles.end()) {
            it = tiles.emplace(
                cacheKey,
                std::make_unique<TilesetTile>(
                    key,
                    scheme->tileToRectangle(key))).first;
        }
        return it->second.get();
    };

    ASSERT_TRUE(TileChildMaterializer::materializeRasterUpsampledChildren(
        parent,
        200.0,
        ensure));
    ASSERT_EQ(4u, parent.children.size());
    TilesetTile* sw = parent.children[0];
    ASSERT_NE(nullptr, sw);
    sw->content.renderContent.prepareGltfContent(
        makeQuadTerrainGltfModel(sw->bounds), Mat4::identity());
    sw->content.renderContent.setTerrainRenderContent(true);
    sw->content.renderContent.addGltfPrimitiveResource(
        GltfPrimitiveRenderResources{});
    sw->content.renderContent.markRenderContentReady();
    RasterMappedToTilesetTile& mapped =
        sw->rasterOverlayState.ensureMapping(0);
    std::vector<RasterOverlayProjection> missingProjections;
    mapped.update(
        sw->key,
        sw->content.renderContent.rasterOverlayDetails(),
        256.0,
        256.0,
        provider,
        nullptr,
        missingProjections);
    ASSERT_NE(nullptr, mapped.getLoadingTile());
    mapped.getLoadingTile()->setTexture(
        std::make_unique<DummyTexture>(4, 4));
    RecordingPrepareRendererResources prep;
    mapped.update(
        sw->key,
        sw->content.renderContent.rasterOverlayDetails(),
        256.0,
        256.0,
        provider,
        &prep,
        missingProjections);
    ASSERT_TRUE(sw->content.renderContent.hasGltfContent());
    ASSERT_EQ(1u, sw->rasterOverlayState.mappingCount());
    ASSERT_EQ(1, prep.attachCount);

    parent.geometricError = 240.0;
    ASSERT_TRUE(TileChildMaterializer::materializeRasterUpsampledChildren(
        parent,
        200.0,
        ensure,
        &prep));

    ASSERT_EQ(4u, parent.children.size());
    EXPECT_EQ(sw, parent.children[0]);
    EXPECT_EQ(scheme->tileToRectangle(sw->key), sw->bounds);
    EXPECT_DOUBLE_EQ(120.0, sw->geometricError);
    EXPECT_FALSE(sw->content.renderContent.hasGltfContent());
    EXPECT_FALSE(sw->content.renderContent.isMeshReady());
    EXPECT_FALSE(sw->content.renderContent.isRenderContentReady());
    EXPECT_EQ(0u, sw->rasterOverlayState.mappingCount());
    EXPECT_EQ(1, prep.detachCount);
    EXPECT_EQ(sw->key, prep.lastDetachedGeometryKey);
    EXPECT_EQ(0, prep.lastDetachedOverlayIndex);
    EXPECT_TRUE(sw->content.renderContent.hasTerrainHeightRange());
    EXPECT_DOUBLE_EQ(-5.0, sw->content.renderContent.terrainMinimumHeight());
    EXPECT_DOUBLE_EQ(25.0, sw->content.renderContent.terrainMaximumHeight());
}

TEST(TileChildMaterializerTest, RasterUpsampledTileCanContinueSubdividingForImageryDetail) {
    TilesetTile parent(
        TileKey{"Geographic-TMS", 10, 512, 512},
        Rectangle::fromDegrees(106.0, 29.0, 107.0, 30.0));
    parent.geometricError = 64.0;
    parent.content.markRasterDetailUpsample();
    parent.content.renderContent.setTerrainHeightRange(100.0, 500.0);

    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;
    auto ensure = [&tiles](const TileKey& key) -> TilesetTile* {
        const std::string cacheKey = cacheKeyFor(key);
        auto it = tiles.find(cacheKey);
        if (it == tiles.end()) {
            it = tiles.emplace(
                cacheKey,
                std::make_unique<TilesetTile>(key, Rectangle{})).first;
        }
        return it->second.get();
    };

    const bool changed =
        TileChildMaterializer::materializeRasterUpsampledChildren(
            parent,
            64.0,
            ensure);

    ASSERT_TRUE(changed);
    ASSERT_EQ(4u, parent.children.size());
    for (TilesetTile* child : parent.children) {
        ASSERT_NE(nullptr, child);
        EXPECT_EQ(&parent, child->parent);
        EXPECT_TRUE(child->content.isRasterDetailUpsample());
        EXPECT_DOUBLE_EQ(32.0, child->geometricError);
        EXPECT_TRUE(child->content.renderContent.hasTerrainHeightRange());
        EXPECT_DOUBLE_EQ(
            100.0,
            child->content.renderContent.terrainMinimumHeight());
        EXPECT_DOUBLE_EQ(
            500.0,
            child->content.renderContent.terrainMaximumHeight());
    }

    EXPECT_TRUE(TileChildMaterializer::canRefine(
        parent,
        TileRefinementAvailabilityOptions{
            true,
            false,
            false,
            false,
            true,
            18},
        [](const TileKey&) { return TileAvailabilityState::NotAvailable; }));
}

TEST(TileChildMaterializerTest,
     RasterUpsampledChildrenKeepDeepQuadtreeIdLikeCesiumNative) {
    auto scheme = TileScheme::createGeographicTMS();
    TilesetTile parent(
        TileKey{"Geographic-TMS", 30, 600000000, 600000000},
        Rectangle::fromDegrees(106.0, 29.0, 107.0, 30.0));
    parent.geometricError = 64.0;
    parent.content.renderContent.setTerrainHeightRange(100.0, 500.0);

    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;
    auto ensure = [&tiles, &scheme](const TileKey& key) -> TilesetTile* {
        const std::string cacheKey = cacheKeyFor(key);
        auto it = tiles.find(cacheKey);
        if (it == tiles.end()) {
            it = tiles.emplace(
                cacheKey,
                std::make_unique<TilesetTile>(
                    key,
                    scheme->tileToRectangle(key))).first;
        }
        return it->second.get();
    };

    const bool changed =
        TileChildMaterializer::materializeRasterUpsampledChildren(
            parent,
            64.0,
            ensure);

    ASSERT_TRUE(changed);
    ASSERT_EQ(4u, parent.children.size());
    EXPECT_EQ((TileKey{"Geographic-TMS", 31, 1200000000, 1200000000}),
              parent.children[0]->key);
    EXPECT_EQ((TileKey{"Geographic-TMS", 31, 1200000001, 1200000000}),
              parent.children[1]->key);
    EXPECT_EQ((TileKey{"Geographic-TMS", 31, 1200000000, 1200000001}),
              parent.children[2]->key);
    EXPECT_EQ((TileKey{"Geographic-TMS", 31, 1200000001, 1200000001}),
              parent.children[3]->key);
    EXPECT_EQ(
        scheme->tileToRectangle(parent.children[0]->key),
        parent.children[0]->bounds);
    EXPECT_EQ(
        scheme->tileToRectangle(parent.children[3]->key),
        parent.children[3]->bounds);
}

TEST(TileChildMaterializerTest,
     RasterUpsampledChildrenDoNotAliasRootWhenTileIdWouldOverflow) {
    TilesetTile parent(
        TileKey{"Geographic-TMS", 31, 1073741824, 1073741824},
        Rectangle::fromDegrees(106.0, 29.0, 107.0, 30.0));
    parent.geometricError = 64.0;

    bool ensureCalled = false;
    auto ensure = [&ensureCalled](const TileKey&) -> TilesetTile* {
        ensureCalled = true;
        return nullptr;
    };

    const bool changed =
        TileChildMaterializer::materializeRasterUpsampledChildren(
            parent,
            64.0,
            ensure);

    EXPECT_FALSE(changed);
    EXPECT_FALSE(ensureCalled);
    EXPECT_TRUE(parent.children.empty());
}

TEST(TileChildMaterializerTest,
     RasterUpsampledChildrenClearStaleNonRasterRenderContentLikeCesiumNative) {
    TilesetTile parent(
        TileKey{"Geographic-TMS", 0, 0, 0},
        Rectangle::fromDegrees(-20.0, -10.0, 0.0, 10.0));
    parent.geometricError = 100.0;
    parent.content.renderContent.setTerrainHeightRange(-5.0, 25.0);

    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;
    auto ensure = [&tiles](const TileKey& key) -> TilesetTile* {
        const std::string cacheKey = cacheKeyFor(key);
        auto it = tiles.find(cacheKey);
        if (it == tiles.end()) {
            it = tiles.emplace(
                cacheKey,
                std::make_unique<TilesetTile>(key, Rectangle{})).first;
        }
        return it->second.get();
    };

    TilesetTile* staleChild =
        ensure(TileKey{"Geographic-TMS", 1, 1, 0});
    ASSERT_NE(nullptr, staleChild);
    staleChild->content.renderContent.prepareGltfContent(
        makeQuadTerrainGltfModel(staleChild->bounds), Mat4::identity());
    staleChild->content.renderContent.setTerrainRenderContent(true);
    staleChild->content.renderContent.addGltfPrimitiveResource(
        GltfPrimitiveRenderResources{});
    staleChild->content.renderContent.markRenderContentReady();

    const bool changed =
        TileChildMaterializer::materializeRasterUpsampledChildren(
            parent,
            100.0,
            ensure);

    ASSERT_TRUE(changed);
    ASSERT_EQ(4u, parent.children.size());
    EXPECT_EQ(staleChild, parent.children[1]);
    EXPECT_TRUE(staleChild->content.isRasterDetailUpsample());
    EXPECT_FALSE(staleChild->content.renderContent.isMeshReady());
    EXPECT_FALSE(staleChild->content.renderContent.hasGltfModel());
    EXPECT_FALSE(staleChild->content.renderContent.hasGltfResources());
    EXPECT_FALSE(staleChild->content.renderContent.isRenderContentReady());
    EXPECT_TRUE(staleChild->content.renderContent.hasTerrainHeightRange());
    EXPECT_DOUBLE_EQ(
        -5.0,
        staleChild->content.renderContent.terrainMinimumHeight());
    EXPECT_DOUBLE_EQ(
        25.0,
        staleChild->content.renderContent.terrainMaximumHeight());
}

TEST(TileChildMaterializerTest,
     RasterUpsampledChildrenUseReadyRasterBeforeGpuTexture) {
    DebugImageryProvider imagery;
    auto scheme = TileScheme::createGeographicTMS();
    RasterOverlayTileProvider provider(imagery, *scheme, nullptr);
    TilesetTile parent(
        TileKey{"Geographic-TMS", 0, 0, 0},
        Rectangle::fromDegrees(-20.0, -10.0, 0.0, 10.0));
    parent.geometricError = 100.0;
    parent.content.renderContent.setTerrainHeightRange(-5.0, 25.0);
    RasterMappedToTilesetTile& mapped =
        addMoreDetailRasterMapping(parent, provider);

    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;
    auto ensure = [&tiles, &scheme](const TileKey& key) -> TilesetTile* {
        const std::string cacheKey = cacheKeyFor(key);
        auto it = tiles.find(cacheKey);
        if (it == tiles.end()) {
            it = tiles.emplace(
                cacheKey,
                std::make_unique<TilesetTile>(
                    key,
                    scheme->tileToRectangle(key))).first;
        }
        return it->second.get();
    };

    EXPECT_TRUE(mapped.isMoreDetailAvailable());
    EXPECT_EQ(
        SurfaceRasterBindingKind::None,
        chooseSurfaceRasterBinding(&mapped).kind);
    EXPECT_TRUE(TileRasterUpsampledChildMaterializer::materialize(
        parent,
        100.0,
        ensure));
    ASSERT_EQ(4u, parent.children.size());
    EXPECT_EQ(
        scheme->tileToRectangle(TileKey{"Geographic-TMS", 1, 0, 0}),
        parent.children[0]->bounds);
    EXPECT_EQ(
        scheme->tileToRectangle(TileKey{"Geographic-TMS", 1, 1, 0}),
        parent.children[1]->bounds);
    EXPECT_EQ(
        scheme->tileToRectangle(TileKey{"Geographic-TMS", 1, 0, 1}),
        parent.children[2]->bounds);
    EXPECT_EQ(
        scheme->tileToRectangle(TileKey{"Geographic-TMS", 1, 1, 1}),
        parent.children[3]->bounds);

    RasterOverlayTile* readyTile = mapped.getReadyTile();
    ASSERT_NE(nullptr, readyTile);
    readyTile->setTexture(std::make_unique<DummyTexture>(4, 4));

    EXPECT_FALSE(TileRasterUpsampledChildMaterializer::materialize(
        parent,
        100.0,
        ensure));
    EXPECT_EQ(4u, parent.children.size());
}

TEST(TileChildMaterializerTest,
     RasterUpsampledChildrenRequireValidBoundingRegionHeightLikeCesiumNative) {
    DebugImageryProvider imagery;
    auto scheme = TileScheme::createGeographicTMS();
    RasterOverlayTileProvider provider(imagery, *scheme, nullptr);
    TilesetTile parent(
        TileKey{"Geographic-TMS", 0, 0, 0},
        Rectangle::fromDegrees(-20.0, -10.0, 0.0, 10.0));
    parent.geometricError = 100.0;
    parent.content.renderContent.prepareGltfContent(
        makeQuadTerrainGltfModel(parent.bounds), Mat4::identity());
    parent.content.renderContent.setTerrainRenderContent(true);
    parent.content.renderContent.addGltfPrimitiveResource(
        GltfPrimitiveRenderResources{});
    parent.content.renderContent.markRenderContentReady();
    parent.content.renderContent.setTerrainHeightRange(-5.0, 25.0);
    RasterOverlayDetails* details =
        parent.content.renderContent.mutableRasterOverlayDetails();
    details->rasterOverlayProjections = {RasterOverlayProjection::Geographic};
    details->rasterOverlayRectangles = {parent.bounds};
    details->boundingRegion = {parent.bounds, 25.0, -5.0};

    auto& mapped = parent.rasterOverlayState.ensureMapping(0);
    std::vector<RasterOverlayProjection> missingProjections;
    mapped.update(
        parent.key,
        parent.content.renderContent.rasterOverlayDetails(),
        256.0,
        256.0,
        provider,
        nullptr,
        missingProjections);
    RasterOverlayTile* loadingTile = mapped.getLoadingTile();
    ASSERT_NE(nullptr, loadingTile);
    loadingTile->setState(RasterOverlayTile::LoadState::Loaded);
    loadingTile->setMoreDetailAvailable(
        RasterOverlayTile::MoreDetailAvailable::Yes);
    EXPECT_EQ(
        RasterMappedToTilesetTile::MoreDetail::Yes,
        mapped.update(
            parent.key,
            parent.content.renderContent.rasterOverlayDetails(),
            256.0,
            256.0,
            provider,
            nullptr,
            missingProjections));
    EXPECT_TRUE(mapped.isMoreDetailAvailable());

    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;
    auto ensure = [&tiles](const TileKey& key) -> TilesetTile* {
        const std::string cacheKey = cacheKeyFor(key);
        auto it = tiles.find(cacheKey);
        if (it == tiles.end()) {
            it = tiles.emplace(
                cacheKey,
                std::make_unique<TilesetTile>(key, Rectangle{})).first;
        }
        return it->second.get();
    };

    EXPECT_FALSE(TileRasterUpsampledChildMaterializer::materialize(
        parent,
        100.0,
        ensure));
    EXPECT_TRUE(parent.children.empty());
    EXPECT_TRUE(tiles.empty());
}

TEST(TileChildMaterializerTest,
     TerrainRasterUpsampleRequiresGltfRenderContentLikeCesiumNative) {
    DebugImageryProvider imagery;
    auto scheme = TileScheme::createGeographicTMS();
    RasterOverlayTileProvider provider(imagery, *scheme, nullptr);
    TilesetTile parent(
        TileKey{"Geographic-TMS", 0, 0, 0},
        Rectangle::fromDegrees(-20.0, -10.0, 0.0, 10.0));
    parent.geometricError = 100.0;

    RasterMappedToTilesetTile& mapped =
        addMoreDetailRasterMapping(parent, provider);
    ASSERT_TRUE(mapped.isMoreDetailAvailable());
    ASSERT_TRUE(parent.content.renderContent.hasGltfContent());
    parent.content.renderContent.setTerrainRenderContent(true);

    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;
    auto ensure = [&tiles](const TileKey& key) -> TilesetTile* {
        const std::string cacheKey = cacheKeyFor(key);
        auto it = tiles.find(cacheKey);
        if (it == tiles.end()) {
            it = tiles.emplace(
                cacheKey,
                std::make_unique<TilesetTile>(key, Rectangle{})).first;
        }
        return it->second.get();
    };

    // With glTF render content, raster upsampling succeeds
    EXPECT_TRUE(TileRasterUpsampledChildMaterializer::materialize(
        parent,
        100.0,
        ensure));
    EXPECT_FALSE(parent.children.empty());
    EXPECT_FALSE(tiles.empty());
}

TEST(TileChildMaterializerTest,
     RasterUpsampledChildrenIgnorePoisonedDetailsRectangle) {
    DebugImageryProvider imagery;
    auto overlayScheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *overlayScheme, nullptr);
    auto scheme = TileScheme::createGeographicTMS();
    TilesetTile parent(
        TileKey{"Geographic-TMS", 2, 2, 1},
        Rectangle::fromDegrees(-30.0, -20.0, 30.0, 20.0));
    parent.geometricError = 80.0;
    parent.content.renderContent.prepareGltfContent(
        makeQuadTerrainGltfModel(parent.bounds), Mat4::identity());
    parent.content.renderContent.setTerrainRenderContent(true);
    parent.content.renderContent.addGltfPrimitiveResource(
        GltfPrimitiveRenderResources{});
    parent.content.renderContent.markRenderContentReady();
    parent.content.renderContent.setTerrainHeightRange(-20.0, 120.0);

    // Content-derived rectangles that deliberately disagree with the
    // parent's real bounds. Such rectangles drift per upsample level and
    // used to poison the children bounding-volume union, frustum-culling
    // visible tiles; the materializer must not let them leak into child
    // bounds.
    const Rectangle tightRegion =
        Rectangle::fromDegrees(-10.0, -10.0, 10.0, 10.0);
    const Rectangle overlayRegion =
        Rectangle::fromDegrees(-20.0, -5.0, 20.0, 15.0);
    RasterOverlayDetails* details =
        parent.content.renderContent.mutableRasterOverlayDetails();
    details->rasterOverlayProjections = {
        RasterOverlayProjection::Geographic,
        RasterOverlayProjection::WebMercator};
    details->rasterOverlayRectangles = {
        tightRegion,
        projectRectangleSimple(
            WebMercatorProjection(Ellipsoid::WGS84()),
            overlayRegion)};
    details->boundingRegion = {tightRegion, -20.0, 120.0};

    auto& mapped = parent.rasterOverlayState.ensureMapping(0);
    std::vector<RasterOverlayProjection> missingProjections;
    EXPECT_EQ(
        RasterMappedToTilesetTile::MoreDetail::Unknown,
        mapped.update(
            parent.key,
            parent.content.renderContent.rasterOverlayDetails(),
            256.0,
            256.0,
            provider,
            nullptr,
            missingProjections));

    RasterOverlayTile* loadingTile = mapped.getLoadingTile();
    ASSERT_NE(nullptr, loadingTile);
    loadingTile->setState(RasterOverlayTile::LoadState::Loaded);
    loadingTile->setMoreDetailAvailable(
        RasterOverlayTile::MoreDetailAvailable::Yes);
    EXPECT_EQ(
        RasterMappedToTilesetTile::MoreDetail::Yes,
        mapped.update(
            parent.key,
            parent.content.renderContent.rasterOverlayDetails(),
            256.0,
            256.0,
            provider,
            nullptr,
            missingProjections));

    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;
    auto ensure = [&tiles, &scheme](const TileKey& key) -> TilesetTile* {
        const std::string cacheKey = cacheKeyFor(key);
        auto it = tiles.find(cacheKey);
        if (it == tiles.end()) {
            it = tiles.emplace(
                cacheKey,
                std::make_unique<TilesetTile>(
                    key,
                    scheme->tileToRectangle(key))).first;
        }
        return it->second.get();
    };

    ASSERT_TRUE(TileRasterUpsampledChildMaterializer::materialize(
        parent,
        80.0,
        ensure));
    ASSERT_EQ(4u, parent.children.size());

    // Children keep the registry/scheme rectangles; neither the details
    // bounding region nor the overlay rectangles influence their bounds.
    for (TilesetTile* child : parent.children) {
        ASSERT_NE(nullptr, child);
        EXPECT_EQ(scheme->tileToRectangle(child->key), child->bounds);
        ASSERT_TRUE(child->boundingVolume.has_value());
        EXPECT_EQ(child->bounds, child->boundingVolume->region);
    }
}

TEST(TileChildMaterializerTest, CanRefineHonorsContentRulesBeforeTerrainSignals) {
    TilesetTile tile(TileKey{"test", 0, 0, 0}, Rectangle{});
    auto noAvailability = [](const TileKey&) {
        return TileAvailabilityState::NotAvailable;
    };

    EXPECT_TRUE(TileChildMaterializer::canRefine(
        tile,
        TileRefinementAvailabilityOptions{
            false,
            true,
            false,
            false,
            false,
            4},
        noAvailability));

    EXPECT_FALSE(TileChildMaterializer::canRefine(
        tile,
        TileRefinementAvailabilityOptions{
            false,
            false,
            true,
            false,
            true,
            4},
        [](const TileKey&) { return TileAvailabilityState::Available; }));
}

TEST(TileChildMaterializerTest, CanRefineIgnoresCachedHeightmapTerrainSignals) {
    TilesetTile tile(TileKey{"Geographic-TMS", 0, 0, 0}, Rectangle{});

    EXPECT_FALSE(TileChildMaterializer::canRefine(
        tile,
        TileRefinementAvailabilityOptions{
            false,
            false,
            false,
            false,
            false,
            4},
        [](const TileKey&) { return TileAvailabilityState::NotAvailable; }));

    EXPECT_TRUE(TileChildMaterializer::canRefine(
        tile,
        TileRefinementAvailabilityOptions{
            false,
            false,
            false,
            false,
            true,
            4},
        [](const TileKey& key) {
            return key.x == 1 && key.y == 0
                ? TileAvailabilityState::Available
                : TileAvailabilityState::NotAvailable;
        }));
}

TEST(TileChildMaterializerTest, CanRefineStopsAtMaxZoomWithoutChildrenOrTerrainSignals) {
    TilesetTile tile(TileKey{"Geographic-TMS", 4, 8, 8}, Rectangle{});

    EXPECT_FALSE(TileChildMaterializer::canRefine(
        tile,
        TileRefinementAvailabilityOptions{
            false,
            false,
            false,
            false,
            true,
            4},
        [](const TileKey&) { return TileAvailabilityState::Available; }));
}

TEST(TileChildMaterializerTest, CanRefineSkipsOutOfRangeGeographicTmsChildren) {
    TilesetTile tile(TileKey{"Geographic-TMS", 0, 2, 0}, Rectangle{});
    int availabilityChecks = 0;

    EXPECT_FALSE(TileChildMaterializer::canRefine(
        tile,
        TileRefinementAvailabilityOptions{
            false,
            false,
            false,
            false,
            true,
            2},
        [&availabilityChecks](const TileKey&) {
            ++availabilityChecks;
            return TileAvailabilityState::Available;
        }));
    EXPECT_EQ(0, availabilityChecks);
}

TEST(TileChildMaterializerTest,
     CanRefineReadsRepresentableDeepGeographicTmsChildren) {
    TilesetTile tile(
        TileKey{"Geographic-TMS", 30, 1073741823, 536870911},
        Rectangle{});
    int availabilityChecks = 0;

    EXPECT_TRUE(TileChildMaterializer::canRefine(
        tile,
        TileRefinementAvailabilityOptions{
            false,
            false,
            false,
            false,
            true,
            31},
        [&availabilityChecks](const TileKey& key) {
            ++availabilityChecks;
            return key == TileKey{"Geographic-TMS", 31, 2147483647, 1073741823}
                ? TileAvailabilityState::Available
                : TileAvailabilityState::NotAvailable;
        }));
    EXPECT_EQ(4, availabilityChecks);
}

TEST(TileChildMaterializerTest,
     CanRefineAllowsExistingChildrenAtAvailabilityBoundaryLikeCesiumNative) {
    TilesetTile tile(TileKey{"Geographic-TMS", 0, 0, 0}, Rectangle{});
    TilesetTile existingChild(TileKey{"Geographic-TMS", 1, 0, 0}, Rectangle{});
    tile.children.push_back(&existingChild);

    EXPECT_TRUE(TileChildMaterializer::canRefine(
        tile,
        TileRefinementAvailabilityOptions{
            true,
            false,
            false,
            true,
            true,
            4},
        [](const TileKey&) { return TileAvailabilityState::Available; }));
}

TEST(TileChildMaterializerTest, CanRefineBlocksTerrainUpsampledTiles) {
    TilesetTile tile(TileKey{"Geographic-TMS", 0, 0, 0}, Rectangle{});
    TilesetTile existingChild(TileKey{"Geographic-TMS", 1, 0, 0}, Rectangle{});
    tile.children.push_back(&existingChild);

    tile.content.markTerrainAvailabilityUpsample();
    EXPECT_FALSE(TileChildMaterializer::canRefine(
        tile,
        TileRefinementAvailabilityOptions{
            true,
            true,
            false,
            false,
            true,
            4},
        [](const TileKey&) { return TileAvailabilityState::Available; }));
}
