// Shadow selection runner（异步选择 步2）单测。
//
// 契约：TileSelectionShadowRunner 在影子树上跑与同步路径逐字相同的
// executor + finalize，产出 tilePlan/loadQueue/counters,全程不碰 live 状态。
// 本步验证绑定机制端到端跑通:确定性、非空且 key 合法、退化帧安全、
// 不改 live registry。与 cesium golden 的逐字等价由 步3
// (test_selector_cesium_golden_diff 的 async 变体) 权威验证。

#include <gtest/gtest.h>

#include "earth_engine/content/GltfContentProvider.h"
#include "earth_engine/core/geodesy/Cartographic.h"
#include "earth_engine/core/geodesy/Ellipsoid.h"
#include "earth_engine/core/math/Rectangle.h"
#include "earth_engine/core/math/Vec3.h"
#include "earth_engine/scene/Camera.h"
#include "earth_engine/scene/FrameState.h"
#include "earth_engine/tiling/RasterMappedToTilesetTile.h"
#include "earth_engine/tiling/TileBoundingVolume.h"
#include "earth_engine/tiling/TileCacheKey.h"
#include "earth_engine/tiling/TileKey.h"
#include "earth_engine/tiling/TileScheme.h"
#include "earth_engine/tiling/TileSelectionShadowRunner.h"
#include "earth_engine/tiling/Tileset.h"
#include "earth_engine/tiling/TilesetTile.h"
#include "earth_engine/tiling/TilesetTileRegistry.h"

#include <memory>
#include <set>
#include <string>
#include <vector>

using namespace earth_engine;

namespace {

constexpr const char* kSchemeId = "Geographic-TMS";

// 最小内容 provider：满 2 层四叉树（root + 4 子），供 runner 取 rootTiles。
class MiniProvider final : public TilesetContentProvider {
public:
    std::string id() const override { return "shadow-runner-mini"; }

    bool supportsTile(const TileKey& key) const override {
        return key.schemeId == kSchemeId && key.z >= 0 && key.z <= 1 &&
               key.x >= 0 && key.x < (1 << key.z) && key.y >= 0 &&
               key.y < (1 << key.z);
    }

    std::vector<TileKey> rootTiles() const override {
        return {TileKey{kSchemeId, 0, 0, 0}};
    }

    std::vector<TileKey> childTiles(const TileKey& key) const override {
        if (!supportsTile(key) || key.z >= 1) {
            return {};
        }
        std::vector<TileKey> children;
        for (int dy = 0; dy <= 1; ++dy) {
            for (int dx = 0; dx <= 1; ++dx) {
                children.push_back(
                    TileKey{kSchemeId, key.z + 1, 2 * key.x + dx,
                            2 * key.y + dy});
            }
        }
        return children;
    }

    void requestTileContent(const TileKey& key,
                            CancellationToken,
                            ContentCallback callback,
                            HttpRequestPriority = HttpRequestPriority::Normal)
        override {
        callback(key, TileContentLoadResult::retryLater());
    }

    TileContentLoadResult decodeContent(const uint8_t*, size_t) override {
        return TileContentLoadResult::failed();
    }
};

TilesetTile* insertLive(TilesetTileRegistry& registry,
                        const TileKey& key,
                        const Rectangle& bounds,
                        TilesetTile* parent) {
    auto tile = std::make_unique<TilesetTile>(key, bounds, parent);
    TilesetTile* raw = tile.get();
    registry.tiles()[TileCacheKey::forTile(key)] = std::move(tile);
    if (parent) {
        parent->children.push_back(raw);
    }
    return raw;
}

// 直接物化一棵 2 层树(root + 4 子)，内容 Done/Empty 便于选择可渲染。
TilesetTileRegistry makeLiveTree() {
    TilesetTileRegistry registry;
    // root 覆盖赤道一带较小区域，便于近天底相机细化。
    const double w = -0.02, s = -0.01, e = 0.02, n = 0.01;
    TilesetTile* root = insertLive(
        registry, TileKey{kSchemeId, 0, 0, 0}, Rectangle(w, s, e, n), nullptr);
    root->geometricError = 500000.0;
    root->refine = TileRefine::Replace;
    root->boundingVolume = TileBoundingVolume::fromRegion(root->bounds, 0.0, 0.0);
    root->content.loadState = TileLoadState::Done;
    root->content.contentKind = TileContentKind::Empty;

    for (int i = 0; i < 4; ++i) {
        const int dx = i % 2;
        const int dy = i / 2;
        const double cw = w + dx * (e - w) * 0.5;
        const double cs = s + dy * (n - s) * 0.5;
        TilesetTile* child = insertLive(
            registry, TileKey{kSchemeId, 1, dx, dy},
            Rectangle(cw, cs, cw + (e - w) * 0.5, cs + (n - s) * 0.5), root);
        child->geometricError = 250000.0;
        child->refine = TileRefine::Replace;
        child->boundingVolume =
            TileBoundingVolume::fromRegion(child->bounds, 0.0, 0.0);
        child->content.loadState = TileLoadState::Done;
        child->content.contentKind = TileContentKind::Empty;
    }
    return registry;
}

SelectorView makeSelectorView(const Camera& camera, int w, int h) {
    SelectorView view;
    view.position = camera.position();
    view.direction = camera.direction();
    view.projectionMatrix = camera.projectionMatrix(w, h);
    view.frustum = Frustum::fromViewProjection(
        view.projectionMatrix * camera.viewMatrix());
    view.viewportHeightPixels = h;
    return view;
}

// 天底相机：置于树中心正上方 height 米，朝椭球中心看。
FrameState makeNadirFrame(Camera& camera,
                          std::vector<SelectorView>& views,
                          double heightMeters) {
    const Vec3 eye = Ellipsoid::WGS84().cartographicToCartesian(
        Cartographic(0.0, 0.0, heightMeters));
    const Vec3 center = Ellipsoid::WGS84().cartographicToCartesian(
        Cartographic(0.0, 0.0, 0.0));
    camera.setPerspective(60.0 * 3.14159265358979323846 / 180.0, 0.1, 1e8);
    camera.lookAt(eye, center, Vec3(0.0, 0.0, 1.0));

    views.push_back(makeSelectorView(camera, 1024, 768));

    FrameState frame;
    frame.frameId = 1;
    frame.camera = &camera;
    frame.viewportWidthPixels = 1024;
    frame.viewportHeightPixels = 768;
    frame.selectorViews = views;
    return frame;
}

TileSelectionShadowRunInput makeInput(const TilesetTileRegistry& live,
                                      const TileScheme& scheme,
                                      const MiniProvider& provider,
                                      const TilesetOptions& options,
                                      const FrameState& frame,
                                      const Camera& camera) {
    TileSelectionShadowRunInput input{
        live,
        scheme,
        &provider,
        /*contentProviderOwnsTerrainQuadtree=*/false,
        /*useVirtualTerrainRoot=*/false,
        options,
        frame,
        camera.position()};
    return input;
}

std::set<std::string> visibleKeySet(const TilePlan& plan) {
    std::set<std::string> keys;
    for (const TileKey& key : plan.visibleTiles) {
        keys.insert(std::to_string(key.z) + "-" + std::to_string(key.x) + "-" +
                    std::to_string(key.y));
    }
    return keys;
}

} // namespace

TEST(TileSelectionShadowRunnerTest, RunsSelectionOnShadowDeterministically) {
    TilesetTileRegistry live = makeLiveTree();
    auto scheme = TileScheme::createGeographicTMS();
    MiniProvider provider;
    TilesetOptions options;

    Camera camera;
    std::vector<SelectorView> views;
    const FrameState frame = makeNadirFrame(camera, views, 3000.0);

    TileSelectionShadowRunner runner1;
    runner1.run(makeInput(live, *scheme, provider, options, frame, camera));

    // 非空 + 每个 visible key 都在 live 树里。
    EXPECT_GT(runner1.counters().visited, 0);
    EXPECT_FALSE(runner1.tilePlan().visibleTiles.empty());
    for (const TileKey& key : runner1.tilePlan().visibleTiles) {
        EXPECT_NE(live.findTile(key), nullptr)
            << "visible key 不在 live 树: z=" << key.z;
    }

    // 确定性：第二个 runner 同输入 → 逐字段一致。
    TileSelectionShadowRunner runner2;
    runner2.run(makeInput(live, *scheme, provider, options, frame, camera));
    EXPECT_EQ(visibleKeySet(runner1.tilePlan()), visibleKeySet(runner2.tilePlan()));
    EXPECT_EQ(runner1.counters().visited, runner2.counters().visited);
    EXPECT_EQ(runner1.counters().culled, runner2.counters().culled);
    EXPECT_EQ(runner1.loadQueue().size(), runner2.loadQueue().size());
}

TEST(TileSelectionShadowRunnerTest, DoesNotMutateLiveRegistry) {
    TilesetTileRegistry live = makeLiveTree();
    auto scheme = TileScheme::createGeographicTMS();
    MiniProvider provider;
    TilesetOptions options;

    // live 瓦片选择态基线快照。
    const size_t liveSizeBefore = live.tiles().size();
    std::vector<TileSelectionState> before;
    for (const auto& e : live.tiles()) {
        before.push_back(e.second->selectionFrameState.selectionState);
    }

    Camera camera;
    std::vector<SelectorView> views;
    const FrameState frame = makeNadirFrame(camera, views, 3000.0);

    TileSelectionShadowRunner runner;
    runner.run(makeInput(live, *scheme, provider, options, frame, camera));

    // live registry 规模不变、选择态不被 runner 触碰（唯一变更者是渲染线程）。
    EXPECT_EQ(live.tiles().size(), liveSizeBefore);
    size_t idx = 0;
    for (const auto& e : live.tiles()) {
        EXPECT_EQ(e.second->selectionFrameState.selectionState, before[idx])
            << "runner 改动了 live 选择态 z=" << e.second->key.z;
        ++idx;
    }
}

TEST(TileSelectionShadowRunnerTest, DegenerateFrameProducesEmptyPlanSafely) {
    TilesetTileRegistry live = makeLiveTree();
    auto scheme = TileScheme::createGeographicTMS();
    MiniProvider provider;
    TilesetOptions options;

    // 无 selectorViews → frame runner 早退，plan 应空且不崩。
    Camera camera;
    camera.setPerspective(1.0, 0.1, 1e8);
    FrameState frame;
    frame.frameId = 1;
    frame.camera = &camera;
    frame.viewportWidthPixels = 1024;
    frame.viewportHeightPixels = 768;

    TileSelectionShadowRunner runner;
    runner.run(makeInput(live, *scheme, provider, options, frame, camera));

    EXPECT_TRUE(runner.tilePlan().visibleTiles.empty());
    EXPECT_EQ(runner.counters().visited, 0);
}
