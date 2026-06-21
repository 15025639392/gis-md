#include <gtest/gtest.h>

#include "earth_engine/content/GltfContentProvider.h"
#include "earth_engine/core/geodesy/Ellipsoid.h"
#include "earth_engine/core/math/Vec3.h"
#include "earth_engine/scene/Camera.h"
#include "earth_engine/scene/FrameState.h"
#include "earth_engine/tiling/TileBoundingVolume.h"
#include "earth_engine/tiling/TileBoundsMetrics.h"
#include "earth_engine/tiling/TileSelectionRasterOverlayPreparer.h"
#include "earth_engine/tiling/TileScheme.h"
#include "earth_engine/tiling/Tileset.h"
#include "earth_engine/tiling/TilesetSelectionFrameFacade.h"

#include <algorithm>
#include <array>
#include <memory>
#include <optional>
#include <string>
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

    static bool isTileRenderable(Tileset& tileset, const TilesetTile& tile) {
        return TileSelectionRasterOverlayPreparer::isRenderable(
            tile,
            tileset.rasterOverlays_);
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
};
} // namespace earth_engine

namespace {

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
        std::unique_ptr<TerrainProvider>{},
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
        std::unique_ptr<TerrainProvider>{},
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
        std::unique_ptr<TerrainProvider>{},
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
        std::unique_ptr<TerrainProvider>{},
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
        std::unique_ptr<TerrainProvider>{},
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
        std::unique_ptr<TerrainProvider>{},
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
        std::unique_ptr<TerrainProvider>{},
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
        std::unique_ptr<TerrainProvider>{},
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
                std::unique_ptr<TerrainProvider>{},
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
        std::unique_ptr<TerrainProvider>{},
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
        std::unique_ptr<TerrainProvider>{},
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
            std::unique_ptr<TerrainProvider>{},
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
        std::unique_ptr<TerrainProvider>{},
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
            std::unique_ptr<TerrainProvider>{},
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
        std::unique_ptr<TerrainProvider>{},
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
        std::unique_ptr<TerrainProvider>{},
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
        std::unique_ptr<TerrainProvider>{},
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
        std::unique_ptr<TerrainProvider>{},
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
        std::unique_ptr<TerrainProvider>{},
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
