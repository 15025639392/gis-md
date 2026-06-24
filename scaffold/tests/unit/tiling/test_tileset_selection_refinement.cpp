#include <gtest/gtest.h>

#include "earth_engine/content/GltfContentProvider.h"
#include "earth_engine/core/geodesy/Cartographic.h"
#include "earth_engine/core/geodesy/Ellipsoid.h"
#include "earth_engine/core/math/Vec3.h"
#include "earth_engine/providers/QuantizedMeshTerrainProvider.h"
#include "earth_engine/scene/Camera.h"
#include "earth_engine/scene/FrameState.h"
#include "earth_engine/tiling/TileCacheKey.h"
#include "earth_engine/tiling/TileBoundingVolume.h"
#include "earth_engine/tiling/TileBoundsMetrics.h"
#include "earth_engine/tiling/TileSelectionRasterOverlayPreparer.h"
#include "earth_engine/tiling/TileRefinementAvailabilityResolver.h"
#include "earth_engine/tiling/TileScheme.h"
#include "earth_engine/tiling/Tileset.h"
#include "earth_engine/tiling/TilesetSelectionFrameFacade.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace earth_engine;

namespace earth_engine {
struct TilesetTestAccess {
    static TilesetTile* ensureTile(Tileset& tileset, const TileKey& key) {
        return tileset.contentAccess_.ensureTile(key);
    }

    static TilesetTile* findTile(Tileset& tileset, const TileKey& key) {
        return tileset.tileRegistry_.findTile(key);
    }

    static void ensureTileChildren(Tileset& tileset, TilesetTile& tile) {
        tileset.contentAccess_.ensureTileChildren(tile);
    }

    static void ensureTileMesh(Tileset& tileset, TilesetTile& tile) {
        tileset.meshPreparation_.prepareRenderableTile(tile);
    }

    static void putTerrainCache(
        Tileset& tileset,
        const TileKey& key,
        std::unique_ptr<DecodedHeightmap> heightmap) {
        tileset.contentLifecycle_.legacyHeightmapTerrainCache()[TileCacheKey::forTile(key)] =
            std::move(heightmap);
    }

    static bool isTileRenderable(Tileset& tileset, const TilesetTile& tile) {
        return TileSelectionRasterOverlayPreparer::isRenderable(
            tile,
            tileset.rasterOverlays_);
    }

    static bool canRefine(Tileset& tileset, const TilesetTile& tile) {
        return tileset.contentAccess_.canRefine(tile);
    }

    static bool loadQueueContainsNormal(
        const Tileset& tileset,
        const TileKey& key) {
        for (const TileLoadRequest& request : tileset.loadQueue_) {
            if (request.key == key &&
                request.group == TileLoadPriorityGroup::Normal) {
                return true;
            }
        }
        return false;
    }

    static bool loadQueueContainsUrgent(
        const Tileset& tileset,
        const TileKey& key) {
        for (const TileLoadRequest& request : tileset.loadQueue_) {
            if (request.key == key &&
                request.group == TileLoadPriorityGroup::Urgent) {
                return true;
            }
        }
        return false;
    }

    static bool loadQueueContainsAny(
        const Tileset& tileset,
        const TileKey& key) {
        for (const TileLoadRequest& request : tileset.loadQueue_) {
            if (request.key == key) {
                return true;
            }
        }
        return false;
    }

    static const TileSelectionCounters& selectionCounters(
        const Tileset& tileset) {
        return tileset.selectionCounters_;
    }

    static Vec3 tileBoundsCenter(const Rectangle& bounds) {
        return TileBoundsMetrics::tileBoundsCenter(bounds);
    }

    static void selectTiles(Tileset& tileset, const FrameState& frameState) {
        TilesetSelectionFrameFacade::selectTiles(tileset, frameState);
    }

    static void setLastCamera(
        Tileset& tileset,
        const Vec3& position,
        const Vec3& direction) {
        tileset.lastCameraPosition_ = position;
        tileset.lastCameraDirection_ = direction;
    }

    static void setOcclusionState(
        Tileset& tileset,
        TileOcclusionState state) {
        tileset.setOcclusionCallback(
            [state](const TilesetTile&) { return state; });
    }

    static void setOcclusionCallback(
        Tileset& tileset,
        Tileset::OcclusionCallback callback) {
        tileset.setOcclusionCallback(std::move(callback));
    }
};
} // namespace earth_engine

namespace {

std::unique_ptr<GltfModel> makeLoadedTerrainGltfModel(
    const Rectangle& rectangle) {
    auto model = std::make_unique<GltfModel>();
    GltfPrimitive primitive;
    primitive.vertices.resize(3);
    primitive.vertices[0].positionEcef = Vec3(0.0, 0.0, 0.0);
    primitive.vertices[1].positionEcef = Vec3(1.0, 0.0, 0.0);
    primitive.vertices[2].positionEcef = Vec3(0.0, 1.0, 0.0);
    for (SurfaceVertex& vertex : primitive.vertices) {
        vertex.normalEcef = Vec3::unitZ();
    }
    primitive.vertices[0].uv = {0.0f, 0.0f};
    primitive.vertices[1].uv = {1.0f, 0.0f};
    primitive.vertices[2].uv = {0.0f, 1.0f};
    primitive.vertexTexCoords[0] = {
        std::array<float, 2>{0.0f, 0.0f},
        std::array<float, 2>{1.0f, 0.0f},
        std::array<float, 2>{0.0f, 1.0f}};
    primitive.indices = {0, 1, 2};
    primitive.runtime.nodeIndex = 0;
    primitive.runtime.baseVertices = primitive.vertices;
    primitive.runtime.hasNormals = true;

    GltfNodeRuntime rootNode;
    rootNode.mesh = 0;
    model->nodes.push_back(rootNode);
    model->sceneRootNodes.push_back(0);
    model->primitives.push_back(std::move(primitive));
    model->rasterOverlayDetails.setGeographicRectangle(rectangle);
    return model;
}

void setLoadedTerrainGltfContent(TilesetTile& tile) {
    tile.content.renderContent.prepareGltfContent(
        makeLoadedTerrainGltfModel(tile.bounds),
        Mat4::identity());
    tile.content.renderContent.setTerrainRenderContent(true);
    tile.markRenderContentDone();
}

class SelectionTreeContentProvider final : public TilesetContentProvider {
public:
    SelectionTreeContentProvider(
        std::vector<TileKey> roots,
        std::vector<std::pair<TileKey, std::vector<TileKey>>> children)
        : roots_(std::move(roots)),
          children_(std::move(children)) {}

    std::string id() const override { return "selection-tree-content"; }

    bool supportsTile(const TileKey& key) const override {
        if (std::find(roots_.begin(), roots_.end(), key) != roots_.end()) {
            return true;
        }
        for (const auto& entry : children_) {
            if (entry.first == key) {
                return true;
            }
            if (std::find(entry.second.begin(), entry.second.end(), key) !=
                entry.second.end()) {
                return true;
            }
        }
        return false;
    }

    std::vector<TileKey> rootTiles() const override { return roots_; }

    std::vector<TileKey> childTiles(const TileKey& key) const override {
        for (const auto& entry : children_) {
            if (entry.first == key) {
                return entry.second;
            }
        }
        return {};
    }

    void requestTileContent(
        const TileKey& key,
        CancellationToken,
        ContentCallback callback,
        HttpRequestPriority = HttpRequestPriority::Normal) override {
        callback(key, TileContentLoadResult::retryLater());
    }

    TileContentLoadResult decodeContent(const uint8_t*, size_t) override {
        return TileContentLoadResult::failed();
    }

private:
    std::vector<TileKey> roots_;
    std::vector<std::pair<TileKey, std::vector<TileKey>>> children_;
};

class TerrainQuadtreeContentProvider final : public TilesetContentProvider {
public:
    std::string id() const override { return "content-terrain-quadtree"; }

    bool supportsTile(const TileKey& key) const override {
        return terrainAvailabilityState(key) ==
               TileAvailabilityState::Available;
    }

    bool providesTerrainQuadtree() const override { return true; }

    std::vector<TileKey> childTiles(const TileKey& key) const override {
        for (const auto& entry : legacyChildTiles) {
            if (entry.first == key) {
                return entry.second;
            }
        }
        return {};
    }

    TileAvailabilityState terrainAvailabilityState(
        const TileKey& key) const override {
        for (const auto& entry : availability) {
            if (entry.first == key) {
                return entry.second;
            }
        }
        return TileAvailabilityState::NotAvailable;
    }

    bool isTerrainAvailabilityBoundaryLevel(int level) const override {
        return boundaryLevel >= 0 && level == boundaryLevel;
    }

    void requestTileContent(
        const TileKey& key,
        CancellationToken,
        ContentCallback callback,
        HttpRequestPriority = HttpRequestPriority::Normal) override {
        callback(key, TileContentLoadResult::retryLater());
    }

    TileContentLoadResult decodeContent(const uint8_t*, size_t) override {
        return TileContentLoadResult::failed();
    }

    std::vector<std::pair<TileKey, TileAvailabilityState>> availability;
    std::vector<std::pair<TileKey, std::vector<TileKey>>> legacyChildTiles;
    int boundaryLevel = -1;
};

class AlwaysAvailableLegacyTerrainProvider final : public TerrainProvider {
public:
    std::string id() const override { return "always-available-legacy"; }
    std::string schemeId() const override { return "Geographic-TMS"; }
    int minZoom() const override { return 0; }
    int maxZoom() const override { return 20; }
    int tileSize() const override { return 256; }

    TileAvailabilityState availabilityState(const TileKey& key) const override {
        ++availabilityQueryCount;
        return key.schemeId == schemeId() ? TileAvailabilityState::Available
                                          : TileAvailabilityState::NotAvailable;
    }

    std::string buildUrl(const TileKey& key) const override {
        return std::to_string(key.z) + "/" + std::to_string(key.x) + "/" +
               std::to_string(key.y) + ".terrain";
    }

    void requestTile(const TileKey& key,
                     CancellationToken,
                     TerrainCallback callback,
                     HttpRequestPriority = HttpRequestPriority::Normal)
        override {
        callback(key, TerrainTileLoadResult::failed());
    }

    std::unique_ptr<DecodedHeightmap> decodeTile(const uint8_t*, size_t)
        override {
        return nullptr;
    }

    mutable int availabilityQueryCount = 0;
};

SelectorView makeSelectorView(
    const Camera& camera,
    int viewportWidth,
    int viewportHeight) {
    SelectorView view;
    view.position = camera.position();
    view.direction = camera.direction();
    const double width = static_cast<double>(viewportWidth);
    const double height = static_cast<double>(viewportHeight);
    view.projectionMatrix = camera.projectionMatrix(width, height);
    view.frustum = Frustum::fromViewProjection(
        view.projectionMatrix * camera.viewMatrix());
    view.viewportHeightPixels = viewportHeight;
    return view;
}

void runUnconditionallyRefinedChildIsNotSelected(TilesetOptions options) {
    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    const TileKey childKey{"Geographic-TMS", 1, 0, 0};
    const TileKey grandchildKey{"Geographic-TMS", 2, 0, 0};

    auto contentProvider = std::make_unique<SelectionTreeContentProvider>(
        std::vector<TileKey>{rootKey},
        std::vector<std::pair<TileKey, std::vector<TileKey>>>{
            {rootKey, {childKey}},
            {childKey, {grandchildKey}}});
    Tileset tileset(
        TileScheme::createGeographicTMS(),
        {},
        nullptr,
        options,
        std::move(contentProvider));

    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    ASSERT_NE(root, nullptr);
    root->content.loadState = TileLoadState::Done;
    root->content.contentKind = TileContentKind::Empty;
    root->refine = TileRefine::Replace;
    root->geometricError = 40000.0;
    const Vec3 center = TilesetTestAccess::tileBoundsCenter(root->bounds);
    root->boundingVolume = TileBoundingVolume::fromSphere(center, 1000000.0);

    TilesetTestAccess::ensureTileChildren(tileset, *root);
    TilesetTile* child = TilesetTestAccess::findTile(tileset, childKey);
    ASSERT_NE(child, nullptr);
    child->content.loadState = TileLoadState::Done;
    child->content.contentKind = TileContentKind::Empty;
    child->refine = TileRefine::Replace;
    child->unconditionallyRefine = true;
    child->geometricError = 0.0;
    child->boundingVolume = TileBoundingVolume::fromSphere(center, 1000000.0);

    TilesetTestAccess::ensureTileChildren(tileset, *child);
    TilesetTile* grandchild =
        TilesetTestAccess::findTile(tileset, grandchildKey);
    ASSERT_NE(grandchild, nullptr);
    grandchild->content.loadState = TileLoadState::Unloaded;
    grandchild->content.contentKind = TileContentKind::Unknown;
    grandchild->geometricError = 0.0;
    grandchild->boundingVolume =
        TileBoundingVolume::fromSphere(center, 1000000.0);

    Camera camera;
    camera.lookAt(
        center + center.normalized() * 1000000.0,
        center,
        Vec3::unitZ());

    FrameState frameState;
    frameState.frameId = 149;
    frameState.camera = &camera;
    frameState.viewportWidthPixels = 800;
    frameState.viewportHeightPixels = 800;
    frameState.selectorViews.push_back(makeSelectorView(camera, 800, 800));
    TilesetTestAccess::setLastCamera(
        tileset,
        camera.position(),
        camera.direction());
    TilesetTestAccess::selectTiles(tileset, frameState);

    const auto& visibleTiles = tileset.tilePlan().visibleTiles;
    const auto visibleEnd = visibleTiles.end();
    EXPECT_NE(std::find(visibleTiles.begin(), visibleEnd, rootKey), visibleEnd);
    EXPECT_EQ(
        std::find(visibleTiles.begin(), visibleEnd, childKey),
        visibleEnd);
    EXPECT_EQ(
        std::find(visibleTiles.begin(), visibleEnd, grandchildKey),
        visibleEnd);
    EXPECT_TRUE(
        TilesetTestAccess::loadQueueContainsAny(tileset, grandchildKey));
}

} // namespace

TEST(
    TilesetSelectionRefinementTest,
    AdditiveRefinementRendersParentAndChildren) {
    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    const std::vector<TileKey> childKeys = {
        {"Geographic-TMS", 1, 0, 0},
        {"Geographic-TMS", 1, 1, 0},
        {"Geographic-TMS", 1, 0, 1}};

    auto contentProvider = std::make_unique<SelectionTreeContentProvider>(
        std::vector<TileKey>{rootKey},
        std::vector<std::pair<TileKey, std::vector<TileKey>>>{
            {rootKey, childKeys}});
    Tileset tileset(
        TileScheme::createGeographicTMS(),
        {},
        nullptr,
        TilesetOptions{},
        std::move(contentProvider));

    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    ASSERT_NE(root, nullptr);
    root->content.loadState = TileLoadState::Done;
    root->content.contentKind = TileContentKind::Empty;
    root->refine = TileRefine::Add;
    root->geometricError = 40000.0;

    TilesetTestAccess::ensureTileChildren(tileset, *root);
    ASSERT_EQ(root->children.size(), childKeys.size());
    for (TilesetTile* child : root->children) {
        ASSERT_NE(child, nullptr);
        child->content.loadState = TileLoadState::Done;
        child->content.contentKind = TileContentKind::Empty;
        child->refine = TileRefine::Replace;
        child->geometricError = 0.0;
    }

    const Vec3 center = TilesetTestAccess::tileBoundsCenter(root->bounds);
    Camera camera;
    camera.lookAt(
        center + center.normalized() * 1000000.0,
        center,
        Vec3::unitZ());

    FrameState frameState;
    frameState.frameId = 143;
    frameState.camera = &camera;
    frameState.viewportWidthPixels = 800;
    frameState.viewportHeightPixels = 800;
    frameState.selectorViews.push_back(makeSelectorView(camera, 800, 800));
    TilesetTestAccess::setLastCamera(
        tileset,
        camera.position(),
        camera.direction());
    TilesetTestAccess::selectTiles(tileset, frameState);

    const auto& visibleTiles = tileset.tilePlan().visibleTiles;
    const auto visibleEnd = visibleTiles.end();
    EXPECT_NE(std::find(visibleTiles.begin(), visibleEnd, rootKey), visibleEnd);
    size_t visibleChildCount = 0;
    for (const TileKey& childKey : childKeys) {
        if (std::find(visibleTiles.begin(), visibleEnd, childKey) !=
            visibleEnd) {
            ++visibleChildCount;
        }
    }
    EXPECT_EQ(visibleChildCount, childKeys.size());
    EXPECT_EQ(tileset.tilePlan().selectionKickedCount, 0);
}

TEST(
    TilesetSelectionRefinementTest,
    UnconditionallyRefinedChildIsNotSelected) {
    runUnconditionallyRefinedChildIsNotSelected(TilesetOptions{});

    TilesetOptions forbidHolesOptions;
    forbidHolesOptions.forbidHoles = true;
    runUnconditionallyRefinedChildIsNotSelected(forbidHolesOptions);
}

TEST(
    TilesetSelectionRefinementTest,
    ExternalWrapperRefinesToChildSelection) {
    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    const TileKey externalKey{"Geographic-TMS", 1, 0, 0};
    const TileKey renderKey{"Geographic-TMS", 2, 0, 0};

    auto contentProvider = std::make_unique<SelectionTreeContentProvider>(
        std::vector<TileKey>{rootKey},
        std::vector<std::pair<TileKey, std::vector<TileKey>>>{
            {rootKey, {externalKey}},
            {externalKey, {renderKey}}});
    Tileset tileset(
        TileScheme::createGeographicTMS(),
        {},
        nullptr,
        TilesetOptions{},
        std::move(contentProvider));

    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    ASSERT_NE(root, nullptr);
    const Vec3 center = TilesetTestAccess::tileBoundsCenter(root->bounds);
    root->content.loadState = TileLoadState::Done;
    root->content.contentKind = TileContentKind::Empty;
    root->refine = TileRefine::Replace;
    root->geometricError = 40000.0;
    root->boundingVolume = TileBoundingVolume::fromSphere(center, 1000000.0);

    TilesetTestAccess::ensureTileChildren(tileset, *root);
    TilesetTile* external = TilesetTestAccess::findTile(tileset, externalKey);
    ASSERT_NE(external, nullptr);
    external->content.loadState = TileLoadState::Done;
    external->content.contentKind = TileContentKind::External;
    external->refine = TileRefine::Replace;
    external->unconditionallyRefine = true;
    external->geometricError = 0.0;
    external->boundingVolume =
        TileBoundingVolume::fromSphere(center, 1000000.0);

    TilesetTestAccess::ensureTileChildren(tileset, *external);
    TilesetTile* render = TilesetTestAccess::findTile(tileset, renderKey);
    ASSERT_NE(render, nullptr);
    render->content.loadState = TileLoadState::Done;
    render->content.contentKind = TileContentKind::Empty;
    render->geometricError = 0.0;
    render->boundingVolume = TileBoundingVolume::fromSphere(center, 1000000.0);

    Camera camera;
    camera.lookAt(
        center + center.normalized() * 1000000.0,
        center,
        Vec3::unitZ());

    FrameState frameState;
    frameState.frameId = 150;
    frameState.camera = &camera;
    frameState.viewportWidthPixels = 800;
    frameState.viewportHeightPixels = 800;
    frameState.selectorViews.push_back(makeSelectorView(camera, 800, 800));
    TilesetTestAccess::setLastCamera(
        tileset,
        camera.position(),
        camera.direction());
    TilesetTestAccess::selectTiles(tileset, frameState);

    const auto& visibleTiles = tileset.tilePlan().visibleTiles;
    const auto visibleEnd = visibleTiles.end();
    EXPECT_EQ(
        std::find(visibleTiles.begin(), visibleEnd, externalKey),
        visibleEnd);
    EXPECT_NE(std::find(visibleTiles.begin(), visibleEnd, renderKey), visibleEnd);
    EXPECT_EQ(
        external->selectionFrameState.selectionState,
        TileSelectionState::Refined);
}

TEST(
    TilesetSelectionRefinementTest,
    ReplaceFailedChildSwitchesFromGrandchildToEmptyHole) {
    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    const std::vector<TileKey> childKeys = {
        {"Geographic-TMS", 1, 0, 0},
        {"Geographic-TMS", 1, 1, 0},
        {"Geographic-TMS", 1, 0, 1},
        {"Geographic-TMS", 1, 1, 1}};
    const TileKey failedChildKey = childKeys.front();
    const TileKey grandchildKey{"Geographic-TMS", 2, 0, 0};

    auto contentProvider = std::make_unique<SelectionTreeContentProvider>(
        std::vector<TileKey>{rootKey},
        std::vector<std::pair<TileKey, std::vector<TileKey>>>{
            {rootKey, childKeys},
            {failedChildKey, {grandchildKey}}});
    Tileset tileset(
        TileScheme::createGeographicTMS(),
        {},
        nullptr,
        TilesetOptions{},
        std::move(contentProvider));

    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    ASSERT_NE(root, nullptr);
    root->content.loadState = TileLoadState::Done;
    root->content.contentKind = TileContentKind::Empty;
    root->refine = TileRefine::Replace;
    root->geometricError = 800000000.0;

    TilesetTestAccess::ensureTileChildren(tileset, *root);
    ASSERT_EQ(root->children.size(), childKeys.size());

    TilesetTile* failedChild =
        TilesetTestAccess::findTile(tileset, failedChildKey);
    ASSERT_NE(failedChild, nullptr);
    failedChild->content.loadState = TileLoadState::Failed;
    failedChild->content.contentKind = TileContentKind::Empty;
    failedChild->geometricError = 200000.0;

    TilesetTestAccess::ensureTileChildren(tileset, *failedChild);
    ASSERT_EQ(failedChild->children.size(), 1u);
    TilesetTile* grandchild =
        TilesetTestAccess::findTile(tileset, grandchildKey);
    ASSERT_NE(grandchild, nullptr);
    grandchild->content.loadState = TileLoadState::Done;
    grandchild->content.contentKind = TileContentKind::Empty;
    grandchild->geometricError = 1.0;

    for (size_t i = 1; i < childKeys.size(); ++i) {
        TilesetTile* sibling =
            TilesetTestAccess::findTile(tileset, childKeys[i]);
        ASSERT_NE(sibling, nullptr);
        sibling->content.loadState = TileLoadState::Done;
        sibling->content.contentKind = TileContentKind::Empty;
        sibling->geometricError = 1.0;
    }

    const Vec3 center =
        TilesetTestAccess::tileBoundsCenter(failedChild->bounds);
    Camera nearCamera;
    nearCamera.lookAt(
        center + center.normalized() * 1000000.0,
        center,
        Vec3::unitZ());

    FrameState nearFrame;
    nearFrame.frameId = 151;
    nearFrame.camera = &nearCamera;
    nearFrame.viewportWidthPixels = 800;
    nearFrame.viewportHeightPixels = 800;
    nearFrame.selectorViews.push_back(
        makeSelectorView(nearCamera, 800, 800));
    TilesetTestAccess::setLastCamera(
        tileset,
        nearCamera.position(),
        nearCamera.direction());
    TilesetTestAccess::selectTiles(tileset, nearFrame);

    const auto& nearVisibleTiles = tileset.tilePlan().visibleTiles;
    const auto nearVisibleEnd = nearVisibleTiles.end();
    EXPECT_EQ(
        std::find(nearVisibleTiles.begin(), nearVisibleEnd, failedChildKey),
        nearVisibleEnd);
    EXPECT_NE(
        std::find(nearVisibleTiles.begin(), nearVisibleEnd, grandchildKey),
        nearVisibleEnd);
    EXPECT_EQ(
        failedChild->selectionFrameState.selectionState,
        TileSelectionState::Refined);

    Camera farCamera;
    farCamera.lookAt(
        center + center.normalized() * 30000000.0,
        center,
        Vec3::unitZ());

    FrameState farFrame;
    farFrame.frameId = 152;
    farFrame.camera = &farCamera;
    farFrame.viewportWidthPixels = 800;
    farFrame.viewportHeightPixels = 800;
    farFrame.selectorViews.push_back(makeSelectorView(farCamera, 800, 800));
    TilesetTestAccess::setLastCamera(
        tileset,
        farCamera.position(),
        farCamera.direction());
    TilesetTestAccess::selectTiles(tileset, farFrame);

    const auto& farVisibleTiles = tileset.tilePlan().visibleTiles;
    const auto farVisibleEnd = farVisibleTiles.end();
    EXPECT_EQ(
        failedChild->selectionFrameState.previousSelectionState,
        TileSelectionState::Refined);
    EXPECT_NE(
        std::find(farVisibleTiles.begin(), farVisibleEnd, failedChildKey),
        farVisibleEnd);
    EXPECT_EQ(
        std::find(farVisibleTiles.begin(), farVisibleEnd, grandchildKey),
        farVisibleEnd);
    EXPECT_EQ(
        failedChild->selectionFrameState.selectionState,
        TileSelectionState::Rendered);

    size_t visibleSiblingCount = 0;
    for (size_t i = 1; i < childKeys.size(); ++i) {
        if (std::find(
                farVisibleTiles.begin(),
                farVisibleEnd,
                childKeys[i]) != farVisibleEnd) {
            ++visibleSiblingCount;
        }
    }
    EXPECT_EQ(visibleSiblingCount, childKeys.size() - 1);
}

TEST(
    TilesetSelectionRefinementTest,
    LoadingDescendantLimitRestoresChildLoadQueue) {
    TilesetOptions options;
    options.loadingDescendantLimit = 1;
    options.forbidHoles = true;

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    const std::vector<TileKey> childKeys = {
        {"Geographic-TMS", 1, 0, 0},
        {"Geographic-TMS", 1, 1, 0},
        {"Geographic-TMS", 1, 0, 1},
        {"Geographic-TMS", 1, 1, 1}};

    auto contentProvider = std::make_unique<SelectionTreeContentProvider>(
        std::vector<TileKey>{rootKey},
        std::vector<std::pair<TileKey, std::vector<TileKey>>>{
            {rootKey, childKeys}});
    Tileset tileset(
        TileScheme::createGeographicTMS(),
        {},
        nullptr,
        options,
        std::move(contentProvider));

    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    ASSERT_NE(root, nullptr);
    root->content.loadState = TileLoadState::Done;
    root->content.contentKind = TileContentKind::Empty;
    root->refine = TileRefine::Replace;
    root->geometricError = 100000000.0;

    TilesetTestAccess::ensureTileChildren(tileset, *root);
    ASSERT_EQ(root->children.size(), childKeys.size());
    for (TilesetTile* child : root->children) {
        ASSERT_NE(child, nullptr);
        child->content.loadState = TileLoadState::Unloaded;
        child->content.contentKind = TileContentKind::Unknown;
        child->geometricError = 1.0;
    }

    const Vec3 center = TilesetTestAccess::tileBoundsCenter(root->bounds);
    Camera camera;
    camera.lookAt(
        center + center.normalized() * 1000000.0,
        center,
        Vec3::unitZ());

    FrameState frameState;
    frameState.frameId = 153;
    frameState.camera = &camera;
    frameState.viewportWidthPixels = 800;
    frameState.viewportHeightPixels = 800;
    frameState.selectorViews.push_back(makeSelectorView(camera, 800, 800));
    TilesetTestAccess::setLastCamera(
        tileset,
        camera.position(),
        camera.direction());
    TilesetTestAccess::selectTiles(tileset, frameState);

    const auto& visibleTiles = tileset.tilePlan().visibleTiles;
    const auto visibleEnd = visibleTiles.end();
    EXPECT_NE(std::find(visibleTiles.begin(), visibleEnd, rootKey), visibleEnd);
    EXPECT_EQ(
        root->selectionFrameState.selectionState,
        TileSelectionState::Rendered);

    bool anyChildVisible = false;
    bool anyChildQueued = false;
    for (const TileKey& childKey : childKeys) {
        anyChildVisible |=
            std::find(visibleTiles.begin(), visibleEnd, childKey) != visibleEnd;
        anyChildQueued |= TilesetTestAccess::loadQueueContainsAny(
            tileset,
            childKey);
    }

    EXPECT_FALSE(anyChildVisible);
    EXPECT_FALSE(anyChildQueued);
    EXPECT_EQ(
        tileset.tilePlan().selectionKickedCount,
        static_cast<int>(childKeys.size()));
    EXPECT_TRUE(TilesetTestAccess::loadQueueContainsNormal(tileset, rootKey));
}

TEST(
    TilesetSelectionRefinementTest,
    UnconditionallyRefineIgnoresSatisfiedSse) {
    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    const std::vector<TileKey> childKeys = {
        {"Geographic-TMS", 1, 0, 0},
        {"Geographic-TMS", 1, 1, 0},
        {"Geographic-TMS", 1, 0, 1},
        {"Geographic-TMS", 1, 1, 1}};

    auto contentProvider = std::make_unique<SelectionTreeContentProvider>(
        std::vector<TileKey>{rootKey},
        std::vector<std::pair<TileKey, std::vector<TileKey>>>{
            {rootKey, childKeys}});
    Tileset tileset(
        TileScheme::createGeographicTMS(),
        {},
        nullptr,
        TilesetOptions{},
        std::move(contentProvider));

    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    ASSERT_NE(root, nullptr);
    root->content.loadState = TileLoadState::Done;
    root->content.contentKind = TileContentKind::Empty;
    root->refine = TileRefine::Replace;
    root->unconditionallyRefine = true;
    root->geometricError = 1.0;

    TilesetTestAccess::ensureTileChildren(tileset, *root);
    ASSERT_EQ(root->children.size(), childKeys.size());
    for (TilesetTile* child : root->children) {
        ASSERT_NE(child, nullptr);
        child->content.loadState = TileLoadState::Done;
        child->content.contentKind = TileContentKind::Empty;
        child->geometricError = 0.0;
    }

    const Vec3 center = TilesetTestAccess::tileBoundsCenter(root->bounds);
    Camera camera;
    camera.lookAt(
        center + center.normalized() * 80000000.0,
        center,
        Vec3::unitZ());

    FrameState frameState;
    frameState.frameId = 154;
    frameState.camera = &camera;
    frameState.viewportWidthPixels = 800;
    frameState.viewportHeightPixels = 800;
    frameState.selectorViews.push_back(makeSelectorView(camera, 800, 800));
    TilesetTestAccess::setLastCamera(
        tileset,
        camera.position(),
        camera.direction());
    TilesetTestAccess::selectTiles(tileset, frameState);

    const auto& visibleTiles = tileset.tilePlan().visibleTiles;
    const auto visibleEnd = visibleTiles.end();
    EXPECT_EQ(std::find(visibleTiles.begin(), visibleEnd, rootKey), visibleEnd);
    EXPECT_EQ(
        root->selectionFrameState.selectionState,
        TileSelectionState::Refined);

    size_t visibleChildCount = 0;
    for (const TileKey& childKey : childKeys) {
        if (std::find(visibleTiles.begin(), visibleEnd, childKey) !=
            visibleEnd) {
            ++visibleChildCount;
        }
    }
    EXPECT_EQ(visibleChildCount, childKeys.size());
}

TEST(
    TilesetSelectionRefinementTest,
    UnconditionalChildDisablesChildrenBoundsCulling) {
    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    const TileKey childKey{"Geographic-TMS", 1, 0, 0};

    auto contentProvider = std::make_unique<SelectionTreeContentProvider>(
        std::vector<TileKey>{rootKey},
        std::vector<std::pair<TileKey, std::vector<TileKey>>>{
            {rootKey, {childKey}}});
    Tileset tileset(
        TileScheme::createGeographicTMS(),
        {},
        nullptr,
        TilesetOptions{},
        std::move(contentProvider));

    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    ASSERT_NE(root, nullptr);
    root->content.loadState = TileLoadState::Done;
    root->content.contentKind = TileContentKind::Empty;
    root->refine = TileRefine::Replace;
    root->geometricError = 40000.0;

    TilesetTestAccess::ensureTileChildren(tileset, *root);
    TilesetTile* child = TilesetTestAccess::findTile(tileset, childKey);
    ASSERT_NE(child, nullptr);
    child->content.loadState = TileLoadState::Unloaded;
    child->content.contentKind = TileContentKind::Unknown;
    child->unconditionallyRefine = true;
    child->geometricError = 0.0;

    const Vec3 center = TilesetTestAccess::tileBoundsCenter(root->bounds);
    Camera camera;
    camera.lookAt(
        center + center.normalized() * 80000000.0,
        center,
        Vec3::unitZ());
    child->bounds = Rectangle(0.25, 0.25, 0.3, 0.3);
    child->boundingVolume = TileBoundingVolume::fromSphere(
        camera.position() - camera.direction() * 2000000.0,
        1000.0);

    FrameState frameState;
    frameState.frameId = 155;
    frameState.camera = &camera;
    frameState.viewportWidthPixels = 800;
    frameState.viewportHeightPixels = 800;
    frameState.selectorViews.push_back(makeSelectorView(camera, 800, 800));
    TilesetTestAccess::setLastCamera(
        tileset,
        camera.position(),
        camera.direction());
    TilesetTestAccess::selectTiles(tileset, frameState);

    const auto& visibleTiles = tileset.tilePlan().visibleTiles;
    const auto visibleEnd = visibleTiles.end();
    EXPECT_NE(std::find(visibleTiles.begin(), visibleEnd, rootKey), visibleEnd);
    EXPECT_EQ(
        root->selectionFrameState.selectionState,
        TileSelectionState::Rendered);
}

TEST(
    TilesetSelectionRefinementTest,
    EmptySelectorViewsShortCircuitTraversal) {
    TilesetOptions options;
    options.renderTilesUnderCamera = false;
    options.preloadSiblings = false;

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    auto contentProvider = std::make_unique<SelectionTreeContentProvider>(
        std::vector<TileKey>{rootKey},
        std::vector<std::pair<TileKey, std::vector<TileKey>>>{});
    Tileset tileset(
        TileScheme::createGeographicTMS(),
        {},
        nullptr,
        options,
        std::move(contentProvider));

    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    ASSERT_NE(root, nullptr);
    root->content.loadState = TileLoadState::Done;
    root->content.contentKind = TileContentKind::Empty;

    const Vec3 center = TilesetTestAccess::tileBoundsCenter(root->bounds);
    Camera camera;
    camera.lookAt(
        center + center.normalized() * 1000000.0,
        center,
        Vec3::unitZ());

    FrameState frameState;
    frameState.frameId = 156;
    frameState.camera = &camera;
    frameState.viewportWidthPixels = 800;
    frameState.viewportHeightPixels = 800;
    TilesetTestAccess::setLastCamera(
        tileset,
        camera.position(),
        camera.direction());
    TilesetTestAccess::selectTiles(tileset, frameState);

    EXPECT_TRUE(frameState.selectorViews.empty());
    EXPECT_TRUE(tileset.tilePlan().visibleTiles.empty());
    EXPECT_EQ(
        root->selectionFrameState.selectionState,
        TileSelectionState::NotVisited);
    EXPECT_FALSE(TilesetTestAccess::loadQueueContainsAny(tileset, rootKey));
}

TEST(
    TilesetSelectionRefinementTest,
    FogDensityTableIsConfigurable) {
    auto runWithFogTable =
        [](std::vector<FogDensityAtHeight> fogDensityTable) {
            TilesetOptions options;
            options.fogDensityTable = std::move(fogDensityTable);

            const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
            auto contentProvider =
                std::make_unique<SelectionTreeContentProvider>(
                    std::vector<TileKey>{rootKey},
                    std::vector<std::pair<TileKey, std::vector<TileKey>>>{});
            Tileset tileset(
                TileScheme::createGeographicTMS(),
                {},
                nullptr,
                options,
                std::move(contentProvider));

            TilesetTile* root =
                TilesetTestAccess::ensureTile(tileset, rootKey);
            root->content.loadState = TileLoadState::Done;
            root->content.contentKind = TileContentKind::Empty;

            const Vec3 center =
                TilesetTestAccess::tileBoundsCenter(root->bounds);
            Camera camera;
            camera.lookAt(
                center + center.normalized() * 100000.0,
                center,
                Vec3::unitZ());

            FrameState frameState;
            frameState.frameId = 157;
            frameState.camera = &camera;
            frameState.viewportWidthPixels = 800;
            frameState.viewportHeightPixels = 800;
            frameState.selectorViews.push_back(
                makeSelectorView(camera, 800, 800));
            TilesetTestAccess::setLastCamera(
                tileset,
                camera.position(),
                camera.direction());
            TilesetTestAccess::selectTiles(tileset, frameState);

            return std::pair<TilePlan, TileSelectionCounters>{
                tileset.tilePlan(),
                TilesetTestAccess::selectionCounters(tileset)};
        };

    const auto [clearPlan, clearCounters] = runWithFogTable({{0.0, 0.0}});
    const auto [densePlan, denseCounters] =
        runWithFogTable({{0.0, 1.0}, {1000000.0, 1.0}});

    EXPECT_FALSE(clearPlan.visibleTiles.empty());
    EXPECT_GT(clearCounters.visited, 0);
    EXPECT_EQ(clearCounters.fogCulled, 0);
    EXPECT_TRUE(densePlan.visibleTiles.empty());
    EXPECT_GT(densePlan.notRenderingNodeCount, 0);
    EXPECT_EQ(denseCounters.visited, 0);
    EXPECT_GT(denseCounters.fogCulled, 0);
}

TEST(
    TilesetSelectionRefinementTest,
    RecordsAncestorMeetsSseForDescendants) {
    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    const std::vector<TileKey> childKeys = {
        {"Geographic-TMS", 1, 0, 0},
        {"Geographic-TMS", 1, 1, 0},
        {"Geographic-TMS", 1, 0, 1},
        {"Geographic-TMS", 1, 1, 1}};

    auto contentProvider = std::make_unique<SelectionTreeContentProvider>(
        std::vector<TileKey>{rootKey},
        std::vector<std::pair<TileKey, std::vector<TileKey>>>{
            {rootKey, childKeys}});
    Tileset tileset(
        TileScheme::createGeographicTMS(),
        {},
        nullptr,
        TilesetOptions{},
        std::move(contentProvider));

    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    ASSERT_NE(root, nullptr);
    root->content.loadState = TileLoadState::Unloaded;
    root->content.contentKind = TileContentKind::Unknown;
    root->refine = TileRefine::Replace;
    root->geometricError = 1.0;
    root->selectionFrameState.selectionState = TileSelectionState::Refined;

    const Vec3 center = TilesetTestAccess::tileBoundsCenter(root->bounds);
    Camera camera;
    camera.lookAt(
        center + center.normalized() * 80000000.0,
        center,
        Vec3::unitZ());

    FrameState frameState;
    frameState.frameId = 158;
    frameState.camera = &camera;
    frameState.viewportWidthPixels = 800;
    frameState.viewportHeightPixels = 800;
    frameState.selectorViews.push_back(makeSelectorView(camera, 800, 800));
    TilesetTestAccess::setLastCamera(
        tileset,
        camera.position(),
        camera.direction());
    TilesetTestAccess::selectTiles(tileset, frameState);

    bool descendantRecorded = false;
    for (const TileSelectionRecord& record :
         tileset.tilePlan().selectionRecords) {
        if (record.key.z > rootKey.z && record.ancestorMeetsSse) {
            descendantRecorded = true;
            break;
        }
    }

    EXPECT_TRUE(descendantRecorded);
    EXPECT_GT(tileset.tilePlan().selectionAncestorMeetsSseCount, 0);
}

TEST(
    TilesetSelectionRefinementTest,
    PreviouslyRefinedUnrenderableParentKeepsDescendants) {
    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    const std::vector<TileKey> childKeys = {
        {"Geographic-TMS", 1, 0, 0},
        {"Geographic-TMS", 1, 1, 0},
        {"Geographic-TMS", 1, 0, 1},
        {"Geographic-TMS", 1, 1, 1}};

    auto contentProvider = std::make_unique<SelectionTreeContentProvider>(
        std::vector<TileKey>{rootKey},
        std::vector<std::pair<TileKey, std::vector<TileKey>>>{
            {rootKey, childKeys}});
    Tileset tileset(
        TileScheme::createGeographicTMS(),
        {},
        nullptr,
        TilesetOptions{},
        std::move(contentProvider));

    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    ASSERT_NE(root, nullptr);
    root->content.loadState = TileLoadState::ContentLoaded;
    root->content.contentKind = TileContentKind::Render;
    root->content.renderContent.setMeshReady(false);
    root->content.renderContent.setSurfaceMesh(nullptr);
    root->refine = TileRefine::Replace;
    root->geometricError = 1.0;
    root->selectionFrameState.selectionState = TileSelectionState::Refined;

    TilesetTestAccess::ensureTileChildren(tileset, *root);
    ASSERT_EQ(root->children.size(), childKeys.size());
    for (TilesetTile* child : root->children) {
        ASSERT_NE(child, nullptr);
        child->content.loadState = TileLoadState::Done;
        child->content.contentKind = TileContentKind::Empty;
        child->geometricError = 0.0;
    }

    const Vec3 center = TilesetTestAccess::tileBoundsCenter(root->bounds);
    Camera camera;
    camera.lookAt(
        center + center.normalized() * 80000000.0,
        center,
        Vec3::unitZ());

    FrameState frameState;
    frameState.frameId = 159;
    frameState.camera = &camera;
    frameState.viewportWidthPixels = 800;
    frameState.viewportHeightPixels = 800;
    frameState.selectorViews.push_back(makeSelectorView(camera, 800, 800));
    TilesetTestAccess::setLastCamera(
        tileset,
        camera.position(),
        camera.direction());
    TilesetTestAccess::selectTiles(tileset, frameState);

    const auto& visibleTiles = tileset.tilePlan().visibleTiles;
    const auto visibleEnd = visibleTiles.end();
    EXPECT_EQ(std::find(visibleTiles.begin(), visibleEnd, rootKey), visibleEnd);
    EXPECT_EQ(
        root->selectionFrameState.selectionState,
        TileSelectionState::Refined);

    size_t visibleChildCount = 0;
    bool everyVisibleChildRecordsAncestorMeetsSse = true;
    for (const TileSelectionRecord& record :
         tileset.tilePlan().selectionRecords) {
        if (std::find(childKeys.begin(), childKeys.end(), record.key) ==
            childKeys.end()) {
            continue;
        }
        if (record.state == TileSelectionState::Rendered) {
            ++visibleChildCount;
            everyVisibleChildRecordsAncestorMeetsSse &=
                record.ancestorMeetsSse;
        }
    }

    EXPECT_EQ(visibleChildCount, childKeys.size());
    EXPECT_TRUE(everyVisibleChildRecordsAncestorMeetsSse);
    EXPECT_TRUE(TilesetTestAccess::loadQueueContainsUrgent(tileset, rootKey));
}

TEST(
    TilesetSelectionRefinementTest,
    UsesLargestSseAcrossSelectorViews) {
    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    const std::vector<TileKey> childKeys = {
        {"Geographic-TMS", 1, 0, 0},
        {"Geographic-TMS", 1, 1, 0},
        {"Geographic-TMS", 1, 0, 1},
        {"Geographic-TMS", 1, 1, 1}};
    auto schemeForCenter = TileScheme::createGeographicTMS();
    const Rectangle rootBounds = schemeForCenter->tileToRectangle(rootKey);
    const Vec3 center = TilesetTestAccess::tileBoundsCenter(rootBounds);

    Camera nearCamera;
    nearCamera.lookAt(
        center + center.normalized() * 1000000.0,
        center,
        Vec3::unitZ());
    Camera farCamera;
    farCamera.lookAt(
        center + center.normalized() * 80000000.0,
        center,
        Vec3::unitZ());

    auto runSelection = [&](std::vector<SelectorView> views) {
        auto contentProvider = std::make_unique<SelectionTreeContentProvider>(
            std::vector<TileKey>{rootKey},
            std::vector<std::pair<TileKey, std::vector<TileKey>>>{
                {rootKey, childKeys}});
        Tileset tileset(
            TileScheme::createGeographicTMS(),
            {},
            nullptr,
            TilesetOptions{},
            std::move(contentProvider));

        TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
        root->content.loadState = TileLoadState::Done;
        root->content.contentKind = TileContentKind::Empty;
        root->refine = TileRefine::Replace;
        root->geometricError = 40000.0;
        TilesetTestAccess::ensureTileChildren(tileset, *root);
        for (TilesetTile* child : root->children) {
            child->content.loadState = TileLoadState::Done;
            child->content.contentKind = TileContentKind::Empty;
            child->geometricError = 0.0;
        }

        FrameState frameState;
        frameState.frameId = 160;
        frameState.camera = &nearCamera;
        frameState.viewportWidthPixels = 800;
        frameState.viewportHeightPixels = 800;
        frameState.selectorViews = std::move(views);
        TilesetTestAccess::setLastCamera(
            tileset,
            nearCamera.position(),
            nearCamera.direction());
        TilesetTestAccess::selectTiles(tileset, frameState);
        return tileset.tilePlan().maxVisibleZoom;
    };

    const int farOnlyZoom =
        runSelection({makeSelectorView(farCamera, 800, 800)});
    const int multiViewZoom = runSelection({
        makeSelectorView(farCamera, 800, 800),
        makeSelectorView(nearCamera, 800, 800)});

    EXPECT_EQ(farOnlyZoom, 0);
    EXPECT_GT(multiViewZoom, farOnlyZoom);
}

TEST(
    TilesetSelectionRefinementTest,
    CullRequiresAllSelectorViewsToMissTile) {
    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    const std::vector<TileKey> childKeys = {
        {"Geographic-TMS", 1, 0, 0},
        {"Geographic-TMS", 1, 1, 0},
        {"Geographic-TMS", 1, 0, 1}};

    auto contentProvider = std::make_unique<SelectionTreeContentProvider>(
        std::vector<TileKey>{rootKey},
        std::vector<std::pair<TileKey, std::vector<TileKey>>>{
            {rootKey, childKeys}});
    Tileset tileset(
        TileScheme::createGeographicTMS(),
        {},
        nullptr,
        TilesetOptions{},
        std::move(contentProvider));

    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    ASSERT_NE(root, nullptr);
    root->content.loadState = TileLoadState::Done;
    root->content.contentKind = TileContentKind::Empty;
    root->refine = TileRefine::Replace;
    root->geometricError = 100000000.0;
    TilesetTestAccess::ensureTileChildren(tileset, *root);
    ASSERT_EQ(root->children.size(), childKeys.size());

    const Vec3 up = Vec3(1.0, 0.0, 0.0);
    const Vec3 east = Vec3(0.0, 1.0, 0.0);
    const Vec3 north = Vec3(0.0, 0.0, 1.0);
    const Vec3 base =
        Ellipsoid::WGS84().cartographicToCartesian(
            Cartographic::fromRadians(0.0, 0.0, 0.0));
    const Vec3 targetA = base;
    const Vec3 targetB = base + east * 100000.0;
    const Vec3 targetC = base - east * 100000.0;

    const std::array<Vec3, 3> centers = {targetA, targetB, targetC};
    for (size_t i = 0; i < childKeys.size(); ++i) {
        TilesetTile* child =
            TilesetTestAccess::findTile(tileset, childKeys[i]);
        ASSERT_NE(child, nullptr);
        child->content.loadState = TileLoadState::Done;
        child->content.contentKind = TileContentKind::Empty;
        child->geometricError = 0.0;
        child->boundingVolume = TileBoundingVolume::fromSphere(
            centers[i],
            1000.0);
    }

    Camera cameraA;
    cameraA.lookAt(targetA + up * 100000.0, targetA, north);
    cameraA.setPerspective(glm::radians(10.0), 1.0, 10000000.0);
    Camera cameraB;
    cameraB.lookAt(targetB + up * 100000.0, targetB, north);
    cameraB.setPerspective(glm::radians(10.0), 1.0, 10000000.0);

    FrameState frameState;
    frameState.frameId = 161;
    frameState.camera = &cameraA;
    frameState.viewportWidthPixels = 800;
    frameState.viewportHeightPixels = 800;
    frameState.selectorViews = {
        makeSelectorView(cameraA, 800, 800),
        makeSelectorView(cameraB, 800, 800)};
    TilesetTestAccess::setLastCamera(
        tileset,
        cameraA.position(),
        cameraA.direction());
    TilesetTestAccess::selectTiles(tileset, frameState);

    const auto& visibleTiles = tileset.tilePlan().visibleTiles;
    const auto visibleEnd = visibleTiles.end();
    EXPECT_NE(
        std::find(visibleTiles.begin(), visibleEnd, childKeys[0]),
        visibleEnd);
    EXPECT_NE(
        std::find(visibleTiles.begin(), visibleEnd, childKeys[1]),
        visibleEnd);
    EXPECT_EQ(
        std::find(visibleTiles.begin(), visibleEnd, childKeys[2]),
        visibleEnd);
    EXPECT_EQ(tileset.tilePlan().selectionOccludedCount, 0);
    EXPECT_GE(tileset.tilePlan().notRenderingNodeCount, 1);
}

TEST(
    TilesetSelectionRefinementTest,
    SseUsesProjectionMatrixScale) {
    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    const std::vector<TileKey> childKeys = {
        {"Geographic-TMS", 1, 0, 0},
        {"Geographic-TMS", 1, 1, 0},
        {"Geographic-TMS", 1, 0, 1},
        {"Geographic-TMS", 1, 1, 1}};
    auto schemeForCenter = TileScheme::createGeographicTMS();
    const Rectangle rootBounds = schemeForCenter->tileToRectangle(rootKey);
    const Vec3 center = TilesetTestAccess::tileBoundsCenter(rootBounds);

    Camera camera;
    camera.lookAt(
        center + center.normalized() * 80000000.0,
        center,
        Vec3::unitZ());

    auto runSelection = [&](SelectorView view) {
        auto contentProvider = std::make_unique<SelectionTreeContentProvider>(
            std::vector<TileKey>{rootKey},
            std::vector<std::pair<TileKey, std::vector<TileKey>>>{
                {rootKey, childKeys}});
        Tileset tileset(
            TileScheme::createGeographicTMS(),
            {},
            nullptr,
            TilesetOptions{},
            std::move(contentProvider));

        TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
        root->content.loadState = TileLoadState::Done;
        root->content.contentKind = TileContentKind::Empty;
        root->refine = TileRefine::Replace;
        root->geometricError = 400000.0;
        TilesetTestAccess::ensureTileChildren(tileset, *root);
        for (TilesetTile* child : root->children) {
            child->content.loadState = TileLoadState::Done;
            child->content.contentKind = TileContentKind::Empty;
            child->geometricError = 0.0;
        }

        FrameState frameState;
        frameState.frameId = 162;
        frameState.camera = &camera;
        frameState.viewportWidthPixels = 800;
        frameState.viewportHeightPixels = 800;
        frameState.selectorViews = {view};
        TilesetTestAccess::setLastCamera(
            tileset,
            camera.position(),
            camera.direction());
        TilesetTestAccess::selectTiles(tileset, frameState);
        return tileset.tilePlan().maxVisibleZoom;
    };

    SelectorView nativeProjection = makeSelectorView(camera, 800, 800);
    SelectorView strongerProjection = nativeProjection;
    strongerProjection.projectionMatrix(1, 1) *= 8.0;

    const int nativeZoom = runSelection(nativeProjection);
    const int strongerProjectionZoom = runSelection(strongerProjection);

    EXPECT_EQ(nativeZoom, 0);
    EXPECT_GT(strongerProjectionZoom, nativeZoom);
}

TEST(
    TilesetSelectionRefinementTest,
    FogUsesEachSelectorViewDensity) {
    TilesetOptions options;
    options.fogDensityTable = {
        {0.0, 1.0},
        {1000000.0, 1.0},
        {10000000.0, 0.0}};

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    auto schemeForCenter = TileScheme::createGeographicTMS();
    const Rectangle rootBounds = schemeForCenter->tileToRectangle(rootKey);
    const Vec3 center = TilesetTestAccess::tileBoundsCenter(rootBounds);

    Camera lowFogCamera;
    lowFogCamera.lookAt(
        center + center.normalized() * 100000.0,
        center,
        Vec3::unitZ());
    Camera highClearCamera;
    highClearCamera.lookAt(
        center + center.normalized() * 80000000.0,
        center,
        Vec3::unitZ());

    auto contentProvider = std::make_unique<SelectionTreeContentProvider>(
        std::vector<TileKey>{rootKey},
        std::vector<std::pair<TileKey, std::vector<TileKey>>>{});
    Tileset tileset(
        TileScheme::createGeographicTMS(),
        {},
        nullptr,
        options,
        std::move(contentProvider));

    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    ASSERT_NE(root, nullptr);
    root->content.loadState = TileLoadState::Done;
    root->content.contentKind = TileContentKind::Empty;

    FrameState frameState;
    frameState.frameId = 163;
    frameState.camera = &lowFogCamera;
    frameState.viewportWidthPixels = 800;
    frameState.viewportHeightPixels = 800;
    frameState.selectorViews = {
        makeSelectorView(lowFogCamera, 800, 800),
        makeSelectorView(highClearCamera, 800, 800)};
    TilesetTestAccess::setLastCamera(
        tileset,
        lowFogCamera.position(),
        lowFogCamera.direction());
    TilesetTestAccess::selectTiles(tileset, frameState);

    EXPECT_FALSE(tileset.tilePlan().visibleTiles.empty());
}

TEST(
    TilesetSelectionRefinementTest,
    ReplaceRefinementStopsWhenParentMeetsSse) {
    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    const std::vector<TileKey> childKeys = {
        {"Geographic-TMS", 1, 0, 0},
        {"Geographic-TMS", 1, 1, 0},
        {"Geographic-TMS", 1, 0, 1},
        {"Geographic-TMS", 1, 1, 1}};

    auto contentProvider = std::make_unique<SelectionTreeContentProvider>(
        std::vector<TileKey>{rootKey},
        std::vector<std::pair<TileKey, std::vector<TileKey>>>{
            {rootKey, childKeys}});
    Tileset tileset(
        TileScheme::createGeographicTMS(),
        {},
        nullptr,
        TilesetOptions{},
        std::move(contentProvider));

    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    ASSERT_NE(root, nullptr);
    root->content.loadState = TileLoadState::Done;
    root->content.contentKind = TileContentKind::Empty;
    root->refine = TileRefine::Replace;
    root->geometricError = 1.0;

    TilesetTestAccess::ensureTileChildren(tileset, *root);
    ASSERT_EQ(root->children.size(), childKeys.size());
    for (TilesetTile* child : root->children) {
        ASSERT_NE(child, nullptr);
        child->content.loadState = TileLoadState::Unloaded;
        child->content.contentKind = TileContentKind::Unknown;
        child->geometricError = 0.0;
    }

    const Vec3 center = TilesetTestAccess::tileBoundsCenter(root->bounds);
    Camera camera;
    camera.lookAt(
        center + center.normalized() * 80000000.0,
        center,
        Vec3::unitZ());

    FrameState frameState;
    frameState.frameId = 145;
    frameState.camera = &camera;
    frameState.viewportWidthPixels = 800;
    frameState.viewportHeightPixels = 800;
    frameState.selectorViews.push_back(makeSelectorView(camera, 800, 800));
    TilesetTestAccess::setLastCamera(
        tileset,
        camera.position(),
        camera.direction());
    TilesetTestAccess::selectTiles(tileset, frameState);

    const auto& visibleTiles = tileset.tilePlan().visibleTiles;
    const auto visibleEnd = visibleTiles.end();
    EXPECT_NE(std::find(visibleTiles.begin(), visibleEnd, rootKey), visibleEnd);
    EXPECT_EQ(
        root->selectionFrameState.selectionState,
        TileSelectionState::Rendered);

    bool anyChildVisible = false;
    bool anyChildQueued = false;
    bool anyChildVisited = false;
    for (const TileKey& childKey : childKeys) {
        anyChildVisible |=
            std::find(visibleTiles.begin(), visibleEnd, childKey) != visibleEnd;
        anyChildQueued |= TilesetTestAccess::loadQueueContainsAny(
            tileset,
            childKey);
        for (const TileSelectionRecord& record :
             tileset.tilePlan().selectionRecords) {
            anyChildVisited |= record.key == childKey;
        }
    }

    EXPECT_FALSE(anyChildVisible);
    EXPECT_FALSE(anyChildQueued);
    EXPECT_FALSE(anyChildVisited);
    EXPECT_TRUE(TilesetTestAccess::loadQueueContainsNormal(tileset, rootKey));
}

TEST(
    TilesetSelectionRefinementTest,
    ReplaceRefinementRendersChildrenWhenReady) {
    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    const std::vector<TileKey> childKeys = {
        {"Geographic-TMS", 1, 0, 0},
        {"Geographic-TMS", 1, 1, 0},
        {"Geographic-TMS", 1, 0, 1},
        {"Geographic-TMS", 1, 1, 1}};

    auto contentProvider = std::make_unique<SelectionTreeContentProvider>(
        std::vector<TileKey>{rootKey},
        std::vector<std::pair<TileKey, std::vector<TileKey>>>{
            {rootKey, childKeys}});
    Tileset tileset(
        TileScheme::createGeographicTMS(),
        {},
        nullptr,
        TilesetOptions{},
        std::move(contentProvider));

    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    ASSERT_NE(root, nullptr);
    root->content.loadState = TileLoadState::Done;
    root->content.contentKind = TileContentKind::Empty;
    root->refine = TileRefine::Replace;
    root->geometricError = 40000.0;

    TilesetTestAccess::ensureTileChildren(tileset, *root);
    ASSERT_EQ(root->children.size(), childKeys.size());
    for (TilesetTile* child : root->children) {
        ASSERT_NE(child, nullptr);
        child->content.loadState = TileLoadState::Done;
        child->content.contentKind = TileContentKind::Empty;
        child->refine = TileRefine::Replace;
        child->geometricError = 0.0;
        ASSERT_TRUE(TilesetTestAccess::isTileRenderable(tileset, *child));
    }

    const Vec3 center = TilesetTestAccess::tileBoundsCenter(root->bounds);
    Camera camera;
    camera.lookAt(
        center + center.normalized() * 1000000.0,
        center,
        Vec3::unitZ());

    FrameState frameState;
    frameState.frameId = 146;
    frameState.camera = &camera;
    frameState.viewportWidthPixels = 800;
    frameState.viewportHeightPixels = 800;
    frameState.selectorViews.push_back(makeSelectorView(camera, 800, 800));
    TilesetTestAccess::setLastCamera(
        tileset,
        camera.position(),
        camera.direction());
    TilesetTestAccess::selectTiles(tileset, frameState);

    const auto& visibleTiles = tileset.tilePlan().visibleTiles;
    const auto visibleEnd = visibleTiles.end();
    EXPECT_EQ(std::find(visibleTiles.begin(), visibleEnd, rootKey), visibleEnd);
    EXPECT_EQ(
        root->selectionFrameState.selectionState,
        TileSelectionState::Refined);

    size_t visibleChildCount = 0;
    for (const TileKey& childKey : childKeys) {
        if (std::find(visibleTiles.begin(), visibleEnd, childKey) !=
            visibleEnd) {
            ++visibleChildCount;
        }
    }
    EXPECT_EQ(visibleChildCount, childKeys.size());
    EXPECT_EQ(tileset.tilePlan().selectionKickedCount, 0);
}

TEST(
    TilesetSelectionRefinementTest,
    ReplaceRefinementFallsBackToParentWhileChildrenLoad) {
    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    const std::vector<TileKey> childKeys = {
        {"Geographic-TMS", 1, 0, 0},
        {"Geographic-TMS", 1, 1, 0},
        {"Geographic-TMS", 1, 0, 1},
        {"Geographic-TMS", 1, 1, 1}};

    auto contentProvider = std::make_unique<SelectionTreeContentProvider>(
        std::vector<TileKey>{rootKey},
        std::vector<std::pair<TileKey, std::vector<TileKey>>>{
            {rootKey, childKeys}});
    Tileset tileset(
        TileScheme::createGeographicTMS(),
        {},
        nullptr,
        TilesetOptions{},
        std::move(contentProvider));

    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    ASSERT_NE(root, nullptr);
    root->content.loadState = TileLoadState::Done;
    root->content.contentKind = TileContentKind::Empty;
    root->refine = TileRefine::Replace;
    root->geometricError = 40000.0;

    TilesetTestAccess::ensureTileChildren(tileset, *root);
    ASSERT_EQ(root->children.size(), childKeys.size());
    for (TilesetTile* child : root->children) {
        ASSERT_NE(child, nullptr);
        child->content.loadState = TileLoadState::Unloaded;
        child->content.contentKind = TileContentKind::Unknown;
        child->geometricError = 0.0;
    }

    const Vec3 center = TilesetTestAccess::tileBoundsCenter(root->bounds);
    Camera camera;
    camera.lookAt(
        center + center.normalized() * 1000000.0,
        center,
        Vec3::unitZ());

    FrameState frameState;
    frameState.frameId = 147;
    frameState.camera = &camera;
    frameState.viewportWidthPixels = 800;
    frameState.viewportHeightPixels = 800;
    frameState.selectorViews.push_back(makeSelectorView(camera, 800, 800));
    TilesetTestAccess::setLastCamera(
        tileset,
        camera.position(),
        camera.direction());
    TilesetTestAccess::selectTiles(tileset, frameState);

    const auto& visibleTiles = tileset.tilePlan().visibleTiles;
    const auto visibleEnd = visibleTiles.end();
    EXPECT_NE(std::find(visibleTiles.begin(), visibleEnd, rootKey), visibleEnd);
    EXPECT_EQ(
        root->selectionFrameState.selectionState,
        TileSelectionState::Rendered);

    bool anyChildVisible = false;
    bool allChildrenQueued = true;
    for (const TileKey& childKey : childKeys) {
        anyChildVisible |=
            std::find(visibleTiles.begin(), visibleEnd, childKey) != visibleEnd;
        allChildrenQueued &= TilesetTestAccess::loadQueueContainsAny(
            tileset,
            childKey);
    }

    EXPECT_FALSE(anyChildVisible);
    EXPECT_TRUE(allChildrenQueued);
    EXPECT_EQ(
        tileset.tilePlan().selectionKickedCount,
        static_cast<int>(childKeys.size()));
}

TEST(
    TilesetSelectionRefinementTest,
    ReplaceRefinementRendersFailedChildrenAsHoles) {
    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    const std::vector<TileKey> childKeys = {
        {"Geographic-TMS", 1, 0, 0},
        {"Geographic-TMS", 1, 1, 0},
        {"Geographic-TMS", 1, 0, 1},
        {"Geographic-TMS", 1, 1, 1}};

    auto contentProvider = std::make_unique<SelectionTreeContentProvider>(
        std::vector<TileKey>{rootKey},
        std::vector<std::pair<TileKey, std::vector<TileKey>>>{
            {rootKey, childKeys}});
    Tileset tileset(
        TileScheme::createGeographicTMS(),
        {},
        nullptr,
        TilesetOptions{},
        std::move(contentProvider));

    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    ASSERT_NE(root, nullptr);
    root->content.loadState = TileLoadState::Done;
    root->content.contentKind = TileContentKind::Empty;
    root->refine = TileRefine::Replace;
    root->geometricError = 40000.0;

    TilesetTestAccess::ensureTileChildren(tileset, *root);
    ASSERT_EQ(root->children.size(), childKeys.size());
    for (TilesetTile* child : root->children) {
        ASSERT_NE(child, nullptr);
        child->content.loadState = TileLoadState::Failed;
        child->content.contentKind = TileContentKind::Empty;
        child->refine = TileRefine::Replace;
        child->geometricError = 0.0;
        ASSERT_TRUE(TilesetTestAccess::isTileRenderable(tileset, *child));
    }

    const Vec3 center = TilesetTestAccess::tileBoundsCenter(root->bounds);
    Camera camera;
    camera.lookAt(
        center + center.normalized() * 1000000.0,
        center,
        Vec3::unitZ());

    FrameState frameState;
    frameState.frameId = 148;
    frameState.camera = &camera;
    frameState.viewportWidthPixels = 800;
    frameState.viewportHeightPixels = 800;
    frameState.selectorViews.push_back(makeSelectorView(camera, 800, 800));
    TilesetTestAccess::setLastCamera(
        tileset,
        camera.position(),
        camera.direction());
    TilesetTestAccess::selectTiles(tileset, frameState);

    const auto& visibleTiles = tileset.tilePlan().visibleTiles;
    const auto visibleEnd = visibleTiles.end();
    EXPECT_EQ(std::find(visibleTiles.begin(), visibleEnd, rootKey), visibleEnd);
    EXPECT_EQ(
        root->selectionFrameState.selectionState,
        TileSelectionState::Refined);

    size_t visibleChildCount = 0;
    for (const TileKey& childKey : childKeys) {
        if (std::find(visibleTiles.begin(), visibleEnd, childKey) !=
            visibleEnd) {
            ++visibleChildCount;
        }
    }
    EXPECT_EQ(visibleChildCount, childKeys.size());
    EXPECT_EQ(tileset.tilePlan().selectionKickedCount, 0);
}

TEST(
    TilesetSelectionRefinementTest,
    AdditiveRefinementRendersFailedChildHoleAndSiblings) {
    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    const std::vector<TileKey> childKeys = {
        {"Geographic-TMS", 1, 0, 0},
        {"Geographic-TMS", 1, 1, 0},
        {"Geographic-TMS", 1, 0, 1}};

    auto contentProvider = std::make_unique<SelectionTreeContentProvider>(
        std::vector<TileKey>{rootKey},
        std::vector<std::pair<TileKey, std::vector<TileKey>>>{
            {rootKey, childKeys}});
    Tileset tileset(
        TileScheme::createGeographicTMS(),
        {},
        nullptr,
        TilesetOptions{},
        std::move(contentProvider));

    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    ASSERT_NE(root, nullptr);
    root->content.loadState = TileLoadState::Done;
    root->content.contentKind = TileContentKind::Empty;
    root->refine = TileRefine::Add;
    root->geometricError = 40000.0;

    TilesetTestAccess::ensureTileChildren(tileset, *root);
    ASSERT_EQ(root->children.size(), childKeys.size());
    for (size_t i = 0; i < root->children.size(); ++i) {
        TilesetTile* child = root->children[i];
        ASSERT_NE(child, nullptr);
        child->refine = TileRefine::Replace;
        child->geometricError = 0.0;
        child->content.contentKind = TileContentKind::Empty;
        child->content.loadState =
            i == 0 ? TileLoadState::Failed : TileLoadState::Done;
    }

    ASSERT_TRUE(TilesetTestAccess::isTileRenderable(tileset, *root));
    for (TilesetTile* child : root->children) {
        ASSERT_NE(child, nullptr);
        EXPECT_TRUE(TilesetTestAccess::isTileRenderable(tileset, *child));
    }

    const Vec3 center = TilesetTestAccess::tileBoundsCenter(root->bounds);
    Camera camera;
    camera.lookAt(
        center + center.normalized() * 1000000.0,
        center,
        Vec3::unitZ());

    FrameState frameState;
    frameState.frameId = 144;
    frameState.camera = &camera;
    frameState.viewportWidthPixels = 800;
    frameState.viewportHeightPixels = 800;
    frameState.selectorViews.push_back(makeSelectorView(camera, 800, 800));
    TilesetTestAccess::setLastCamera(
        tileset,
        camera.position(),
        camera.direction());
    TilesetTestAccess::selectTiles(tileset, frameState);

    const auto& visibleTiles = tileset.tilePlan().visibleTiles;
    const auto visibleEnd = visibleTiles.end();
    EXPECT_NE(std::find(visibleTiles.begin(), visibleEnd, rootKey), visibleEnd);
    size_t visibleChildCount = 0;
    for (const TileKey& childKey : childKeys) {
        if (std::find(visibleTiles.begin(), visibleEnd, childKey) !=
            visibleEnd) {
            ++visibleChildCount;
        }
    }
    EXPECT_EQ(visibleChildCount, childKeys.size());
    EXPECT_EQ(tileset.tilePlan().selectionKickedCount, 0);
}

TEST(
    TilesetSelectionRefinementTest,
    OcclusionStopsRefinementBeforeChildLoads) {
    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    const TileKey childKey{"Geographic-TMS", 1, 0, 0};

    auto contentProvider = std::make_unique<SelectionTreeContentProvider>(
        std::vector<TileKey>{rootKey},
        std::vector<std::pair<TileKey, std::vector<TileKey>>>{
            {rootKey, {childKey}}});
    Tileset tileset(
        TileScheme::createGeographicTMS(),
        {},
        nullptr,
        TilesetOptions{},
        std::move(contentProvider));

    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    ASSERT_NE(root, nullptr);
    root->content.loadState = TileLoadState::Done;
    root->content.contentKind = TileContentKind::Empty;
    root->refine = TileRefine::Replace;
    root->geometricError = 40000.0;

    const Vec3 center = TilesetTestAccess::tileBoundsCenter(root->bounds);
    root->boundingVolume = TileBoundingVolume::fromSphere(center, 1000000.0);
    Camera camera;
    camera.lookAt(
        center + center.normalized() * 1000000.0,
        center,
        Vec3::unitZ());

    FrameState frameState;
    frameState.frameId = 150;
    frameState.camera = &camera;
    frameState.viewportWidthPixels = 800;
    frameState.viewportHeightPixels = 800;
    frameState.selectorViews.push_back(makeSelectorView(camera, 800, 800));
    TilesetTestAccess::setLastCamera(
        tileset,
        camera.position(),
        camera.direction());
    TilesetTestAccess::setOcclusionState(
        tileset,
        TileOcclusionState::Occluded);
    TilesetTestAccess::selectTiles(tileset, frameState);

    const auto& visibleTiles = tileset.tilePlan().visibleTiles;
    EXPECT_NE(
        std::find(visibleTiles.begin(), visibleTiles.end(), rootKey),
        visibleTiles.end());
    EXPECT_TRUE(root->children.empty());
    EXPECT_EQ(tileset.tilePlan().maxVisibleZoom, 0);
    EXPECT_EQ(tileset.tilePlan().selectionOccludedCount, 1);
}

TEST(
    TilesetSelectionRefinementTest,
    OcclusionUnavailableDelaysNewRefinement) {
    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    const TileKey childKey{"Geographic-TMS", 1, 0, 0};

    auto contentProvider = std::make_unique<SelectionTreeContentProvider>(
        std::vector<TileKey>{rootKey},
        std::vector<std::pair<TileKey, std::vector<TileKey>>>{
            {rootKey, {childKey}}});
    Tileset tileset(
        TileScheme::createGeographicTMS(),
        {},
        nullptr,
        TilesetOptions{},
        std::move(contentProvider));

    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    ASSERT_NE(root, nullptr);
    root->content.loadState = TileLoadState::Done;
    root->content.contentKind = TileContentKind::Empty;
    root->refine = TileRefine::Replace;
    root->geometricError = 40000.0;

    const Vec3 center = TilesetTestAccess::tileBoundsCenter(root->bounds);
    root->boundingVolume = TileBoundingVolume::fromSphere(center, 1000000.0);
    Camera camera;
    camera.lookAt(
        center + center.normalized() * 1000000.0,
        center,
        Vec3::unitZ());

    FrameState frameState;
    frameState.frameId = 151;
    frameState.camera = &camera;
    frameState.viewportWidthPixels = 800;
    frameState.viewportHeightPixels = 800;
    frameState.selectorViews.push_back(makeSelectorView(camera, 800, 800));
    TilesetTestAccess::setLastCamera(
        tileset,
        camera.position(),
        camera.direction());
    TilesetTestAccess::setOcclusionState(
        tileset,
        TileOcclusionState::OcclusionUnavailable);
    TilesetTestAccess::selectTiles(tileset, frameState);

    const auto& visibleTiles = tileset.tilePlan().visibleTiles;
    EXPECT_NE(
        std::find(visibleTiles.begin(), visibleTiles.end(), rootKey),
        visibleTiles.end());
    EXPECT_TRUE(root->children.empty());
    EXPECT_EQ(tileset.tilePlan().maxVisibleZoom, 0);
    EXPECT_EQ(tileset.tilePlan().selectionWaitingForOcclusionResultsCount, 1);
}

TEST(
    TilesetSelectionRefinementTest,
    OcclusionUnavailableDoesNotDelayPreviouslyRefinedTile) {
    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    const TileKey childKey{"Geographic-TMS", 1, 0, 0};

    auto contentProvider = std::make_unique<SelectionTreeContentProvider>(
        std::vector<TileKey>{rootKey},
        std::vector<std::pair<TileKey, std::vector<TileKey>>>{
            {rootKey, {childKey}}});
    Tileset tileset(
        TileScheme::createGeographicTMS(),
        {},
        nullptr,
        TilesetOptions{},
        std::move(contentProvider));

    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    ASSERT_NE(root, nullptr);
    root->content.loadState = TileLoadState::Done;
    root->content.contentKind = TileContentKind::Empty;
    root->refine = TileRefine::Replace;
    root->geometricError = 40000.0;
    root->selectionFrameState.selectionState = TileSelectionState::Refined;
    TilesetTestAccess::ensureTileChildren(tileset, *root);
    TilesetTile* child = TilesetTestAccess::findTile(tileset, childKey);
    ASSERT_NE(child, nullptr);
    child->content.loadState = TileLoadState::Done;
    child->content.contentKind = TileContentKind::Empty;
    child->refine = TileRefine::Replace;
    child->geometricError = 0.0;

    const Vec3 center = TilesetTestAccess::tileBoundsCenter(root->bounds);
    root->boundingVolume = TileBoundingVolume::fromSphere(center, 1000000.0);
    Camera camera;
    camera.lookAt(
        center + center.normalized() * 1000000.0,
        center,
        Vec3::unitZ());

    FrameState frameState;
    frameState.frameId = 152;
    frameState.camera = &camera;
    frameState.viewportWidthPixels = 800;
    frameState.viewportHeightPixels = 800;
    frameState.selectorViews.push_back(makeSelectorView(camera, 800, 800));
    TilesetTestAccess::setLastCamera(
        tileset,
        camera.position(),
        camera.direction());
    TilesetTestAccess::setOcclusionState(
        tileset,
        TileOcclusionState::OcclusionUnavailable);
    TilesetTestAccess::selectTiles(tileset, frameState);

    EXPECT_FALSE(root->children.empty());
    EXPECT_GT(tileset.tilePlan().maxVisibleZoom, 0);
    EXPECT_EQ(tileset.tilePlan().selectionWaitingForOcclusionResultsCount, 0);
}

TEST(
    TilesetSelectionRefinementTest,
    OcclusionUnavailableChildDelaysNewRefinement) {
    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    const std::vector<TileKey> childKeys = {
        {"Geographic-TMS", 1, 0, 0},
        {"Geographic-TMS", 1, 1, 0}};

    auto contentProvider = std::make_unique<SelectionTreeContentProvider>(
        std::vector<TileKey>{rootKey},
        std::vector<std::pair<TileKey, std::vector<TileKey>>>{
            {rootKey, childKeys}});
    Tileset tileset(
        TileScheme::createGeographicTMS(),
        {},
        nullptr,
        TilesetOptions{},
        std::move(contentProvider));

    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    ASSERT_NE(root, nullptr);
    root->content.loadState = TileLoadState::Done;
    root->content.contentKind = TileContentKind::Empty;
    root->refine = TileRefine::Replace;
    root->geometricError = 40000.0;
    TilesetTestAccess::ensureTileChildren(tileset, *root);
    ASSERT_EQ(root->children.size(), childKeys.size());

    std::unordered_map<TileKey, TileOcclusionState> states;
    states[rootKey] = TileOcclusionState::NotOccluded;
    for (const TilesetTile* child : root->children) {
        ASSERT_NE(child, nullptr);
        states[child->key] = TileOcclusionState::Occluded;
    }
    states[root->children.front()->key] =
        TileOcclusionState::OcclusionUnavailable;
    TilesetTestAccess::setOcclusionCallback(
        tileset,
        [states](const TilesetTile& tile) {
            auto it = states.find(tile.key);
            return it == states.end()
                ? TileOcclusionState::NotOccluded
                : it->second;
        });

    const Vec3 center = TilesetTestAccess::tileBoundsCenter(root->bounds);
    root->boundingVolume = TileBoundingVolume::fromSphere(center, 1000000.0);
    Camera camera;
    camera.lookAt(
        center + center.normalized() * 1000000.0,
        center,
        Vec3::unitZ());

    FrameState frameState;
    frameState.frameId = 153;
    frameState.camera = &camera;
    frameState.viewportWidthPixels = 800;
    frameState.viewportHeightPixels = 800;
    frameState.selectorViews.push_back(makeSelectorView(camera, 800, 800));
    TilesetTestAccess::setLastCamera(
        tileset,
        camera.position(),
        camera.direction());
    TilesetTestAccess::selectTiles(tileset, frameState);

    const bool childrenNotVisited = std::all_of(
        root->children.begin(),
        root->children.end(),
        [](const TilesetTile* child) {
            return child &&
                   child->selectionFrameState.selectionState ==
                       TileSelectionState::NotVisited;
        });
    EXPECT_TRUE(childrenNotVisited);
    EXPECT_EQ(tileset.tilePlan().selectionWaitingForOcclusionResultsCount, 1);
}

TEST(
    TilesetSelectionRefinementTest,
    CameraInsideDiagnosticUsesExplicitRootVolume) {
    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    auto contentProvider = std::make_unique<SelectionTreeContentProvider>(
        std::vector<TileKey>{rootKey},
        std::vector<std::pair<TileKey, std::vector<TileKey>>>{});
    Tileset tileset(
        TileScheme::createGeographicTMS(),
        {},
        nullptr,
        TilesetOptions{},
        std::move(contentProvider));

    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    ASSERT_NE(root, nullptr);
    root->content.loadState = TileLoadState::Done;
    root->content.contentKind = TileContentKind::Empty;
    root->bounds = Rectangle{2.0, 2.0, 3.0, 3.0};
    root->boundingVolume =
        TileBoundingVolume::fromRegion(
            Rectangle{-0.25, -0.25, 0.25, 0.25},
            0.0,
            0.0);

    const Cartographic cameraCartographic(0.0, 0.0, 1000000.0);
    const Vec3 cameraPosition =
        Ellipsoid::WGS84().cartographicToCartesian(cameraCartographic);
    const Vec3 target =
        Ellipsoid::WGS84().cartographicToCartesian(
            Cartographic(0.0, 0.0, 0.0));
    Camera camera;
    camera.lookAt(cameraPosition, target, Vec3::unitZ());

    FrameState frameState;
    frameState.frameId = 154;
    frameState.camera = &camera;
    frameState.viewportWidthPixels = 800;
    frameState.viewportHeightPixels = 800;
    frameState.selectorViews.push_back(makeSelectorView(camera, 800, 800));
    TilesetTestAccess::setLastCamera(
        tileset,
        camera.position(),
        camera.direction());
    TilesetTestAccess::selectTiles(tileset, frameState);

    EXPECT_NE(
        root->selectionFrameState.selectionState,
        TileSelectionState::NotVisited);
    EXPECT_TRUE(root->selectionFrameState.cameraInside);
}

TEST(
    TilesetSelectionRefinementTest,
    AvailabilityBoundaryChildrenWaitForLoadedTerrainContentLikeCesiumNative) {
    auto provider = std::make_unique<QuantizedMeshTerrainProvider>(
        "https://example.invalid/fallback/{z}/{x}/{y}.terrain");
    const std::string layerJson = R"json({
      "format": "quantized-mesh-1.0",
      "projection": "EPSG:4326",
      "scheme": "tms",
      "tiles": ["{z}/{x}/{y}.terrain"],
      "minzoom": 0,
      "maxzoom": 4,
      "metadataAvailability": 1
    })json";

    ASSERT_TRUE(provider->configureFromLayerJson(
        layerJson,
        "https://example.invalid/layer.json"));
    QuantizedMeshTerrainProvider* providerPtr = provider.get();

    Tileset tileset(
        TileScheme::createGeographicTMS(),
        {},
        nullptr,
        TilesetOptions{},
        std::move(provider));

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    ASSERT_NE(root, nullptr);

    TilesetTestAccess::ensureTileChildren(tileset, *root);
    EXPECT_TRUE(root->children.empty());

    setLoadedTerrainGltfContent(*root);

    for (TileLoadState state : {
             TileLoadState::Unloaded,
             TileLoadState::ContentLoading,
             TileLoadState::FailedTemporarily}) {
        root->content.loadState = state;
        TilesetTestAccess::ensureTileChildren(tileset, *root);
        EXPECT_TRUE(root->children.empty());
    }

    root->content.loadState = TileLoadState::ContentLoaded;
    QuantizedMeshAvailabilityUpdate update;
    update.layerIndex = 0;
    update.subtreeKey = rootKey;
    update.metadataAvailability = {{0, 0, 0, 0, 0}};
    providerPtr->applyAvailabilityUpdates({update});
    TilesetTestAccess::ensureTileChildren(tileset, *root);
    EXPECT_EQ(4u, root->children.size());

    root->children.clear();
    root->content.loadState = TileLoadState::Done;
    TilesetTestAccess::ensureTileChildren(tileset, *root);
    EXPECT_EQ(4u, root->children.size());

    root->children.clear();
    root->content.loadState = TileLoadState::Failed;
    TilesetTestAccess::ensureTileChildren(tileset, *root);
    EXPECT_EQ(4u, root->children.size());
}

TEST(
    TilesetSelectionRefinementTest,
    AvailabilityMiddleOfSubtreeCreatesChildrenWithoutWaitingLikeCesiumNative) {
    auto provider = std::make_unique<QuantizedMeshTerrainProvider>(
        "https://example.invalid/fallback/{z}/{x}/{y}.terrain");
    const std::string layerJson = R"json({
      "format": "quantized-mesh-1.0",
      "projection": "EPSG:4326",
      "scheme": "tms",
      "tiles": ["{z}/{x}/{y}.terrain"],
      "minzoom": 0,
      "maxzoom": 4,
      "metadataAvailability": 2
    })json";

    ASSERT_TRUE(provider->configureFromLayerJson(
        layerJson,
        "https://example.invalid/layer.json"));

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    const TileKey middleKey{"Geographic-TMS", 1, 0, 0};
    QuantizedMeshAvailabilityUpdate update;
    update.layerIndex = 0;
    update.subtreeKey = rootKey;
    update.metadataAvailability = {
        {0, 0, 0, 0, 0},
        {1, 0, 0, 0, 0}};
    provider->applyAvailabilityUpdates({update});

    Tileset tileset(
        TileScheme::createGeographicTMS(),
        {},
        nullptr,
        TilesetOptions{},
        std::move(provider));

    TilesetTile* middle = TilesetTestAccess::ensureTile(tileset, middleKey);
    ASSERT_NE(middle, nullptr);
    setLoadedTerrainGltfContent(*middle);

    for (TileLoadState state : {
             TileLoadState::Unloaded,
             TileLoadState::ContentLoading,
             TileLoadState::FailedTemporarily}) {
        middle->children.clear();
        middle->content.loadState = state;
        TilesetTestAccess::ensureTileChildren(tileset, *middle);
        EXPECT_EQ(4u, middle->children.size());
    }
}

TEST(
    TilesetSelectionRefinementTest,
    UnderlyingLayerAvailabilityBoundaryWaitsForLoadedTerrainLikeCesiumNative) {
    const std::filesystem::path rootPath =
        std::filesystem::temp_directory_path() /
        "earth_md_qm_underlying_boundary_refinement_test";
    std::filesystem::remove_all(rootPath);
    std::filesystem::create_directories(rootPath / "parent");

    const std::string parentLayerJson = R"json({
      "format": "quantized-mesh-1.0",
      "projection": "EPSG:4326",
      "scheme": "tms",
      "tiles": ["parentTiles/{z}/{x}/{y}.terrain"],
      "maxzoom": 4,
      "metadataAvailability": 1
    })json";
    {
        std::ofstream out(rootPath / "parent" / "layer.json");
        out << parentLayerJson;
    }

    auto provider = std::make_unique<QuantizedMeshTerrainProvider>(
        "https://example.invalid/fallback/{z}/{x}/{y}.terrain");
    const std::string childLayerJson = R"json({
      "format": "quantized-mesh-1.0",
      "projection": "EPSG:4326",
      "scheme": "tms",
      "tiles": ["childTiles/{z}/{x}/{y}.terrain"],
      "parentUrl": "../parent",
      "maxzoom": 4,
      "available": [
        [{"startX":0,"startY":0,"endX":1,"endY":0}]
      ]
    })json";

    ASSERT_TRUE(provider->configureFromLayerJson(
        childLayerJson,
        "file://" + (rootPath / "child" / "layer.json").generic_string()));
    QuantizedMeshTerrainProvider* providerPtr = provider.get();

    Tileset tileset(
        TileScheme::createGeographicTMS(),
        {},
        nullptr,
        TilesetOptions{},
        std::move(provider));

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    ASSERT_NE(root, nullptr);
    setLoadedTerrainGltfContent(*root);

    for (TileLoadState state : {
             TileLoadState::Unloaded,
             TileLoadState::ContentLoading,
             TileLoadState::FailedTemporarily}) {
        root->children.clear();
        root->content.loadState = state;
        TilesetTestAccess::ensureTileChildren(tileset, *root);
        EXPECT_TRUE(root->children.empty());
    }

    root->content.loadState = TileLoadState::ContentLoaded;
    TilesetTestAccess::ensureTileChildren(tileset, *root);
    EXPECT_TRUE(root->children.empty());

    QuantizedMeshAvailabilityUpdate parentSubtreeUpdate;
    parentSubtreeUpdate.layerIndex = 1;
    parentSubtreeUpdate.subtreeKey = rootKey;
    parentSubtreeUpdate.metadataAvailability = {{0, 0, 0, 0, 0}};
    providerPtr->applyAvailabilityUpdates({parentSubtreeUpdate});

    TilesetTestAccess::ensureTileChildren(tileset, *root);
    EXPECT_EQ(4u, root->children.size());

    std::filesystem::remove_all(rootPath);
}

TEST(
    TilesetSelectionRefinementTest,
    ExplicitQuantizedMeshAvailabilityRefinesPastLayerMaxzoomLikeCesiumNative) {
    auto provider = std::make_unique<QuantizedMeshTerrainProvider>(
        "https://example.invalid/fallback/{z}/{x}/{y}.terrain");
    const std::string layerJson = R"json({
      "format": "quantized-mesh-1.0",
      "projection": "EPSG:4326",
      "scheme": "tms",
      "tiles": ["{z}/{x}/{y}.terrain"],
      "minzoom": 0,
      "maxzoom": 1,
      "available": [
        [{"startX":0,"startY":0,"endX":1,"endY":0}],
        [{"startX":0,"startY":0,"endX":0,"endY":0}],
        [{"startX":0,"startY":0,"endX":0,"endY":0}]
      ]
    })json";

    ASSERT_TRUE(provider->configureFromLayerJson(
        layerJson,
        "https://example.invalid/layer.json"));
    EXPECT_TRUE(provider->supportsTile(TileKey{"Geographic-TMS", 2, 0, 0}));

    Tileset tileset(
        TileScheme::createGeographicTMS(),
        {},
        nullptr,
        TilesetOptions{},
        std::move(provider));
    TilesetTile* parent =
        TilesetTestAccess::ensureTile(
            tileset,
            TileKey{"Geographic-TMS", 1, 0, 0});
    ASSERT_NE(parent, nullptr);

    EXPECT_TRUE(TilesetTestAccess::canRefine(tileset, *parent));

    TilesetTestAccess::ensureTileChildren(tileset, *parent);
    ASSERT_FALSE(parent->children.empty());
    ASSERT_NE(nullptr, parent->children[0]);
    EXPECT_FALSE(parent->children[0]->content.upsampledFromParent);
}

TEST(
    TilesetSelectionRefinementTest,
    ContentTerrainQuadtreeDrivesAvailabilityRefinementWithoutTerrainProvider) {
    auto contentProvider = std::make_unique<TerrainQuadtreeContentProvider>();
    contentProvider->boundaryLevel = 0;
    contentProvider->availability.push_back(
        {TileKey{"Geographic-TMS", 1, 0, 0},
         TileAvailabilityState::Available});
    contentProvider->legacyChildTiles.push_back(
        {TileKey{"Geographic-TMS", 0, 0, 0},
         {TileKey{"Geographic-TMS", 1, 1, 1}}});

    Tileset tileset(
        TileScheme::createGeographicTMS(),
        {},
        nullptr,
        TilesetOptions{},
        std::move(contentProvider));

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    ASSERT_NE(root, nullptr);
    root->geometricError = 100.0;
    root->refine = TileRefine::Replace;

    EXPECT_FALSE(TilesetTestAccess::canRefine(tileset, *root));
    TilesetTestAccess::ensureTileChildren(tileset, *root);
    EXPECT_TRUE(root->children.empty());

    setLoadedTerrainGltfContent(*root);
    EXPECT_TRUE(TilesetTestAccess::canRefine(tileset, *root));

    TilesetTestAccess::ensureTileChildren(tileset, *root);
    ASSERT_EQ(4u, root->children.size());
    EXPECT_EQ((TileKey{"Geographic-TMS", 1, 0, 0}), root->children[0]->key);
    EXPECT_EQ((TileKey{"Geographic-TMS", 1, 1, 0}), root->children[1]->key);
    EXPECT_EQ((TileKey{"Geographic-TMS", 1, 0, 1}), root->children[2]->key);
    EXPECT_EQ((TileKey{"Geographic-TMS", 1, 1, 1}), root->children[3]->key);
    EXPECT_FALSE(root->children[0]->content.isTerrainAvailabilityUpsample());
    EXPECT_TRUE(root->children[1]->content.isTerrainAvailabilityUpsample());
    EXPECT_TRUE(root->children[2]->content.isTerrainAvailabilityUpsample());
    EXPECT_TRUE(root->children[3]->content.isTerrainAvailabilityUpsample());
}

TEST(
    TilesetSelectionRefinementTest,
    ContentTerrainQuadtreeIgnoresLegacyHeightmapTerrainCacheForRefinement) {
    auto contentProvider = std::make_unique<TerrainQuadtreeContentProvider>();

    Tileset tileset(
        TileScheme::createGeographicTMS(),
        {},
        nullptr,
        TilesetOptions{},
        std::move(contentProvider));

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    const TileKey cachedLegacyChildKey{"Geographic-TMS", 1, 0, 0};
    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    ASSERT_NE(root, nullptr);
    root->geometricError = 100.0;
    root->refine = TileRefine::Replace;
    setLoadedTerrainGltfContent(*root);

    TilesetTestAccess::putTerrainCache(
        tileset,
        cachedLegacyChildKey,
        std::make_unique<DecodedHeightmap>());

    EXPECT_FALSE(TilesetTestAccess::canRefine(tileset, *root));
    TilesetTestAccess::ensureTileChildren(tileset, *root);
    EXPECT_TRUE(root->children.empty());
}

TEST(
    TilesetSelectionRefinementTest,
    ResolverIgnoresLegacyHeightmapTerrainCacheForRefinement) {
    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    const TileKey cachedHeightmapChildKey{"Geographic-TMS", 1, 0, 0};
    SelectionTreeContentProvider nonTerrainContentProvider(
        {rootKey},
        {{rootKey, {cachedHeightmapChildKey}}});
    auto tileScheme = TileScheme::createGeographicTMS();
    TilesetTile root(
        rootKey,
        tileScheme->tileToRectangle(rootKey));
    root.geometricError = 100.0;
    setLoadedTerrainGltfContent(root);

    auto canRefineWithProvider =
        [&](const TilesetContentProvider* provider) {
            return TileRefinementAvailabilityResolver::
                canRefineLegacyHeightmapSurfaceOrExternalContent(
                root,
                provider,
                nullptr,
                *tileScheme,
                [](const TilesetTile&) { return false; },
                [](const TilesetTile& tile) {
                    return tile.content.renderContent
                        .hasRenderableTerrainContent();
                });
        };

    EXPECT_FALSE(canRefineWithProvider(nullptr));
    EXPECT_TRUE(canRefineWithProvider(&nonTerrainContentProvider));
}
