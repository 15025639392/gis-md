// 真异步选择路径（asyncSelectionNonBlocking）的非-TSAN 覆盖（步5/步6）。
//
// TSAN 目标验证并发无竞争,但需专用构建;此测进常规 ctest,验证:
//  (1) 真异步路径连跑多帧不崩、协议不变量断言(worker.dispatch/buildShadow/
//      tryTakeResult 的 render 线程亲和 + !isBusy + occlusion==null)在 Debug
//      下成立;
//  (2) 静止相机下,真异步结果(延迟≥1帧)在若干帧后**收敛到与同步选择相同**
//      的 visibleTiles —— 即异步不改变最终选择,只延迟交付。

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
#include "earth_engine/tiling/TileSelectionWorker.h"

#include <memory>
#include <set>
#include <string>
#include <thread>
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
    // Deterministically drain the async worker: spin until it publishes its
    // result, so the next selectTiles() consumes it. Avoids timing-dependent
    // flakiness in a tight test loop where the worker may not schedule in time.
    static void waitForSelectionIdle(Tileset& tileset) {
        if (tileset.selectionWorker_) {
            while (tileset.selectionWorker_->isBusy()) {
                std::this_thread::yield();
            }
        }
    }
};
} // namespace earth_engine

namespace {

constexpr const char* kSchemeId = "Geographic-TMS";
constexpr int kMaxDepth = 3;

class SpecProvider final : public TilesetContentProvider {
public:
    std::string id() const override { return "async-quadtree"; }
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

std::unique_ptr<Tileset> makeTileset(bool nonBlocking) {
    TilesetOptions options;
    options.asyncSelection = true;
    options.asyncSelectionNonBlocking = nonBlocking;
    return std::make_unique<Tileset>(
        TileScheme::createGeographicTMS(),
        std::vector<ActivatedRasterOverlay*>{},
        nullptr,
        std::move(options),
        std::make_unique<SpecProvider>());
}

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

FrameState makeStaticFrame(Camera& camera,
                           std::vector<SelectorView>& views,
                           uint64_t frameId) {
    const Vec3 eye = Ellipsoid::WGS84().cartographicToCartesian(
        Cartographic(0.0, 0.0, 4000.0));
    const Vec3 center = Ellipsoid::WGS84().cartographicToCartesian(
        Cartographic(0.0, 0.0, 0.0));
    camera.setPerspective(60.0 * 3.14159265358979323846 / 180.0, 0.1, 1e8);
    camera.lookAt(eye, center, Vec3(0.0, 0.0, 1.0));
    views.clear();
    views.push_back(makeView(camera, 1024, 768));

    FrameState frame;
    frame.frameId = frameId;
    frame.camera = &camera;
    frame.viewportWidthPixels = 1024;
    frame.viewportHeightPixels = 768;
    frame.selectorViews = views;
    return frame;
}

std::set<std::string> visibleKeys(const Tileset& tileset) {
    std::set<std::string> keys;
    for (const TileKey& key : tileset.tilePlan().visibleTiles) {
        keys.insert(std::to_string(key.z) + "-" + std::to_string(key.x) + "-" +
                    std::to_string(key.y));
    }
    return keys;
}

void runStaticFrame(Tileset& tileset, uint64_t frameId) {
    Camera camera;
    std::vector<SelectorView> views;
    const FrameState frame = makeStaticFrame(camera, views, frameId);
    TilesetTestAccess::setLastCamera(
        tileset, camera.position(), camera.direction());
    TilesetTestAccess::selectTiles(tileset, frame);
}

// One async step: dispatch this frame's selection, then wait for the worker so
// the NEXT call consumes it (deterministic, no reliance on scheduling speed).
void runStaticFrameDrained(Tileset& tileset, uint64_t frameId) {
    runStaticFrame(tileset, frameId);
    TilesetTestAccess::waitForSelectionIdle(tileset);
}

} // namespace

// 静止相机下:真异步(延迟交付)在若干帧后收敛到与同步影子选择相同的
// visibleTiles。异步只延迟、不改变最终选择。
TEST(TilesetAsyncSelectionTest, NonBlockingConvergesToSyncSelection) {
    // 两侧都跑到静止不动点(选择有跨帧历史,须同样帧数收敛后再比,
    // 否则单帧 sync 与多帧 async 的历史态不同会误判)。
    std::unique_ptr<Tileset> syncTs = makeTileset(/*nonBlocking=*/false);
    materialize(*syncTs);
    for (uint64_t f = 1; f <= 64; ++f) {
        runStaticFrame(*syncTs, f);
    }
    const std::set<std::string> syncKeys = visibleKeys(*syncTs);
    EXPECT_FALSE(syncKeys.empty());

    std::unique_ptr<Tileset> asyncTs = makeTileset(/*nonBlocking=*/true);
    materialize(*asyncTs);
    // 每帧 dispatch 后等 worker 完成,下一帧消费(延迟≥1帧)。跑够帧数让
    // 跨帧历史收敛到静止不动点,再补一帧消费最后一个结果。
    for (uint64_t f = 1; f <= 64; ++f) {
        runStaticFrameDrained(*asyncTs, f);
    }
    runStaticFrame(*asyncTs, 65);  // consume the last drained result
    EXPECT_EQ(visibleKeys(*asyncTs), syncKeys)
        << "真异步静止相机未收敛到同步选择";
}

// 真异步路径连跑多帧无崩溃,最终产出非空计划(协议不变量断言在 Debug 下生效)。
TEST(TilesetAsyncSelectionTest, NonBlockingManyFramesProducesPlan) {
    std::unique_ptr<Tileset> ts = makeTileset(/*nonBlocking=*/true);
    materialize(*ts);
    for (uint64_t f = 1; f <= 32; ++f) {
        runStaticFrameDrained(*ts, f);
    }
    runStaticFrame(*ts, 33);  // consume the last drained result
    EXPECT_GT(ts->tilePlan().frameId, 0u);
    EXPECT_FALSE(ts->tilePlan().visibleTiles.empty());
}