// 异步选择线程竞争压测（ThreadSanitizer 目标，步4b）。
//
// 仅在 -DEARTH_ENGINE_ENABLE_TSAN=ON 时编译,链接 tsan-instrumented core。
// 形态:asyncSelection=true 的 Tileset,物化一棵满四叉树,相机每帧扰动强制
// worker 每帧真跑,连跑数千帧。选择在专用 worker 线程上进行(步4 barrier 版:
// snapshot 入/result 出跨线程),reconcile 写回 live。TSAN 报任何 data race /
// 锁序问题即 fail —— 验证 worker handoff 的 happens-before 正确、跨帧写回与
// 下一帧快照之间无竞争。
//
// 步4 为 barrier(render 阻塞等 worker),故理论上无并发重叠;此测的价值是
// 验证 mutex/cv handoff 建立了正确的 happens-before(若同步写错,TSAN 会在
// worker 写 runner_ 与 render 读 runner_ 之间报竞争)。步5 去 barrier 后此
// 测转为真并发的权威门禁。

#include <gtest/gtest.h>

#include "earth_engine/content/GltfContentProvider.h"
#include "earth_engine/core/geodesy/Cartographic.h"
#include "earth_engine/core/geodesy/Ellipsoid.h"
#include "earth_engine/core/math/Rectangle.h"
#include "earth_engine/core/math/Vec3.h"
#include "earth_engine/scene/Camera.h"
#include "earth_engine/scene/FrameState.h"
#include "earth_engine/tiling/TileBoundingVolume.h"
#include "earth_engine/tiling/TileKey.h"
#include "earth_engine/tiling/TileScheme.h"
#include "earth_engine/tiling/Tileset.h"
#include "earth_engine/tiling/TilesetSelectionFrameFacade.h"

#include <cmath>
#include <memory>
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
    static void selectTiles(Tileset& tileset, const FrameState& frameState) {
        TilesetSelectionFrameFacade::selectTiles(tileset, frameState);
    }
    static void setLastCamera(Tileset& tileset,
                              const Vec3& position,
                              const Vec3& direction) {
        tileset.lastCameraPosition_ = position;
        tileset.lastCameraDirection_ = direction;
    }
};
} // namespace earth_engine

namespace {

constexpr const char* kSchemeId = "Geographic-TMS";
constexpr int kMaxDepth = 3;

class SpecProvider final : public TilesetContentProvider {
public:
    std::string id() const override { return "tsan-quadtree"; }
    bool supportsTile(const TileKey& key) const override {
        return key.schemeId == kSchemeId && key.z >= 0 && key.z <= kMaxDepth &&
               key.x >= 0 && key.x < (1 << key.z) && key.y >= 0 &&
               key.y < (1 << key.z);
    }
    std::vector<TileKey> rootTiles() const override {
        return {TileKey{kSchemeId, 0, 0, 0}};
    }
    std::vector<TileKey> childTiles(const TileKey& key) const override {
        if (!supportsTile(key) || key.z >= kMaxDepth) {
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

void materialize(Tileset& tileset) {
    struct Pending {
        TilesetTile* tile;
        double w, s, e, n;
    };
    TilesetTile* root =
        TilesetTestAccess::ensureTile(tileset, TileKey{kSchemeId, 0, 0, 0});
    std::vector<Pending> stack{{root, -0.04, -0.02, 0.04, 0.02}};
    while (!stack.empty()) {
        Pending p = stack.back();
        stack.pop_back();
        p.tile->bounds = Rectangle(p.w, p.s, p.e, p.n);
        p.tile->geometricError = 500000.0 / (1 << p.tile->key.z);
        p.tile->refine = TileRefine::Replace;
        p.tile->boundingVolume =
            TileBoundingVolume::fromRegion(p.tile->bounds, 0.0, 0.0);
        p.tile->content.loadState = TileLoadState::Done;
        p.tile->content.contentKind = TileContentKind::Empty;
        if (p.tile->key.z >= kMaxDepth) {
            continue;
        }
        TilesetTestAccess::ensureTileChildren(tileset, *p.tile);
        const double mw = (p.w + p.e) * 0.5;
        const double ms = (p.s + p.n) * 0.5;
        for (TilesetTile* child : p.tile->children) {
            const int dx = child->key.x - 2 * p.tile->key.x;
            const int dy = child->key.y - 2 * p.tile->key.y;
            stack.push_back(Pending{
                child,
                dx == 0 ? p.w : mw,
                dy == 0 ? p.s : ms,
                dx == 0 ? mw : p.e,
                dy == 0 ? ms : p.n});
        }
    }
}

SelectorView makeView(const Camera& camera, int w, int h) {
    SelectorView view;
    view.position = camera.position();
    view.direction = camera.direction();
    view.projectionMatrix = camera.projectionMatrix(w, h);
    view.frustum = Frustum::fromViewProjection(
        view.projectionMatrix * camera.viewMatrix());
    view.viewportHeightPixels = h;
    return view;
}

} // namespace

TEST(AsyncSelectionTsanTest, PerturbedCameraStressNoRace) {
    TilesetOptions options;
    options.asyncSelection = true;
    Tileset tileset(
        TileScheme::createGeographicTMS(),
        {},
        nullptr,
        std::move(options),
        std::make_unique<SpecProvider>());
    materialize(tileset);

    // 每帧扰动相机高度(+微小经纬抖动),强制 worker 每帧真跑不同选择。
    const int frames = 2000;
    for (int i = 0; i < frames; ++i) {
        const double t = static_cast<double>(i);
        const double height = 1500.0 + 60000.0 * (0.5 + 0.5 * std::sin(t * 0.05));
        const double lon = 0.001 * std::sin(t * 0.11);
        const double lat = 0.0005 * std::cos(t * 0.07);
        const Vec3 eye = Ellipsoid::WGS84().cartographicToCartesian(
            Cartographic(lon, lat, height));
        const Vec3 center = Ellipsoid::WGS84().cartographicToCartesian(
            Cartographic(lon, lat, 0.0));

        Camera camera;
        camera.setPerspective(60.0 * 3.14159265358979323846 / 180.0, 0.1, 1e8);
        camera.lookAt(eye, center, Vec3(0.0, 0.0, 1.0));

        FrameState frame;
        frame.frameId = static_cast<uint64_t>(i) + 1;
        frame.camera = &camera;
        frame.viewportWidthPixels = 1024;
        frame.viewportHeightPixels = 768;
        frame.selectorViews.push_back(makeView(camera, 1024, 768));
        TilesetTestAccess::setLastCamera(
            tileset, camera.position(), camera.direction());
        TilesetTestAccess::selectTiles(tileset, frame);
    }

    // 到达此处即无 crash;真正的门禁是 TSAN 运行时报告(any race → 非零退出)。
    EXPECT_GT(tileset.tilePlan().frameId, 0u);
}
