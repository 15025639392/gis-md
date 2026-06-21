#include <gtest/gtest.h>

#include "earth_engine/content/GltfContentProvider.h"
#include "earth_engine/core/math/Vec3.h"
#include "earth_engine/scene/Camera.h"
#include "earth_engine/scene/FrameState.h"
#include "earth_engine/tiling/TileBoundsMetrics.h"
#include "earth_engine/tiling/TileScheme.h"
#include "earth_engine/tiling/Tileset.h"
#include "earth_engine/tiling/TilesetSelectionFrameFacade.h"

#include <algorithm>
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

    static void ensureTileChildren(Tileset& tileset, TilesetTile& tile) {
        tileset.contentAccess_.ensureTileChildren(tile);
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
