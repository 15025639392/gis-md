// 缩放不露底(zoom-out never reveals a hole / 漏底)—— 回归锁。
//
// 背景:低空久驻后外拉到高空,曾出现"露底/黑块"。根因是 base-coverage 三柱:
//   ① 根层常驻     —— z<=kPinnedBaseCoverageMaxZoom(3) 的瓦片豁免预算驱逐,
//                     祖先链不被 LRU 拆光,"任意瓦片总有可画祖先"。
//                     已被 test_tile_content_cache_manager.cpp 的
//                     BaseCoveragePinBlocksBudgetEvictionQueue 锁住。
//   ② 启动预载种子 —— pinBaseCoverage 开启时,update() 会把全球
//                     z0..kBaseCoveragePreloadMaxZoom(2) 整格(Geographic-TMS
//                     = 42 片)种入加载队列(Preload 组,不与视野竞争),让从未
//                     进过视野的经度也有粗底可画。 << 本文件锁这一柱 >>
//   ③ 影像小泵     —— 预载瓦片不进渲染集,常规 prefetch 永不轮到,故 update()
//                     每帧单独推进其 base 影像(notex 洞的实体)。 << 本文件也锁
//                     这一柱的 novelty >>:泵把**从未进过视野**的粗瓦片的 base
//                     影像推向就绪(建 mapping + 发起影像加载)。见下方 T2。
//
// 本文件两组锁:
//  T1(柱②,device-free):种子是否**与视野无关地**覆盖全球 z0..2。
//  T2(柱③,device+overlay):泵是否为从未访问的粗瓦片建立 base 影像 mapping 并
//     发起加载(常规渲染集 prefetch 永远够不到视野外瓦片,这正是 d22611c73 加的)。
//     ⚠️边界:T2 锁到"影像加载已发起(mapping+loading tile)"为止,**不**断言影像
//     GPU 纹理最终 ready(=drop=0 像素级)。raster 纹理上传级在当前测试 harness
//     无 mock 通路(所有 raster 测试都手塞 setTexture),那一级是引擎通用上传、
//     非本修复 novelty,不在此锁。
//  两组都双向测(pinned 生效 / unpinned 不生效),后者即内建"关掉修复会响",不靠截图。

#include <gtest/gtest.h>

#include "earth_engine/content/GltfContentProvider.h"
#include "earth_engine/content/GltfModel.h"
#include "earth_engine/core/geodesy/Ellipsoid.h"
#include "earth_engine/core/math/Vec3.h"
#include "earth_engine/layers/ActivatedRasterOverlay.h"
#include "earth_engine/layers/RasterOverlay.h"
#include "earth_engine/providers/DebugImageryProvider.h"
#include "earth_engine/scene/Camera.h"
#include "earth_engine/scene/FrameState.h"
#include "earth_engine/tiling/RasterMappedToTilesetTile.h"
#include "earth_engine/tiling/TileBaseCoveragePin.h"
#include "earth_engine/tiling/TileBoundsMetrics.h"
#include "earth_engine/tiling/TilePlan.h"
#include "earth_engine/tiling/TileRasterOverlayReadinessPolicy.h"
#include "earth_engine/tiling/TileScheme.h"
#include "earth_engine/tiling/Tileset.h"

#include "../../helpers/MockRenderDevice.h"

#include <memory>
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
    // The priority group `key` currently sits at in the load queue, or -1 if it
    // is not queued (already dispatched or never seeded).
    static int queuedGroupOf(const Tileset& tileset, const TileKey& key) {
        for (const TileLoadRequest& r : tileset.loadQueue_) {
            if (r.key == key) return static_cast<int>(r.group);
        }
        return -1;
    }
};
} // namespace earth_engine

namespace {

// A provider that never completes (retryLater) and reports the whole shallow
// pyramid as available. We only exercise selection + load-queue seeding, so no
// content ever needs to finish — the tree state stays exactly what update()
// materialized, which is what the seeding invariant is about.
class InertTerrainProvider final : public TilesetContentProvider {
public:
    InertTerrainProvider(std::vector<TileKey> roots, int maxZoom)
        : roots_(std::move(roots)), maxZoom_(maxZoom) {}
    std::string id() const override { return "inert-terrain"; }
    bool supportsTile(const TileKey& key) const override {
        return key.z <= maxZoom_;
    }
    std::vector<TileKey> rootTiles() const override { return roots_; }
    bool providesTerrainQuadtree() const override { return true; }
    TileAvailabilityState availabilityState(const TileKey& key) const override {
        return key.z <= maxZoom_ ? TileAvailabilityState::Available
                                 : TileAvailabilityState::NotAvailable;
    }
    bool isTerrainAvailabilityBoundaryLevel(int) const override {
        return false;
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
    int maxZoom_;
};

SelectorView makeSelectorView(const Camera& camera, int w, int h) {
    SelectorView view;
    view.position = camera.position();
    view.direction = camera.direction();
    view.projectionMatrix = camera.projectionMatrix((double)w, (double)h);
    view.frustum = Frustum::fromViewProjection(
        view.projectionMatrix * camera.viewMatrix());
    view.viewportHeightPixels = h;
    return view;
}

struct Fixture {
    // Geographic-TMS roots.
    const TileKey west{"Geographic-TMS", 0, 0, 0};
    const TileKey east{"Geographic-TMS", 0, 1, 0};
    std::unique_ptr<TileScheme> enumScheme = TileScheme::createGeographicTMS();
    std::unique_ptr<Tileset> tileset;

    explicit Fixture(bool pinBaseCoverage, uint32_t maxSimultaneousLoads = 20) {
        TilesetOptions options;
        options.pinBaseCoverage = pinBaseCoverage;
        options.maximumSimultaneousTileLoads = maxSimultaneousLoads;
        auto provider = std::make_unique<InertTerrainProvider>(
            std::vector<TileKey>{west, east}, /*maxZoom=*/4);
        tileset = std::make_unique<Tileset>(
            TileScheme::createGeographicTMS(),
            std::vector<ActivatedRasterOverlay*>{},
            /*device=*/nullptr,
            options,
            std::move(provider));
    }

    // Drive a single low-altitude frame staring straight down at the WEST root's
    // center — a narrow view that, on its own, would only ever touch tiles near
    // the west. Anything materialized in the EAST is there because of preload,
    // not because the camera saw it.
    void narrowLowFrame(uint64_t frameId) {
        TilesetTile* westTile = TilesetTestAccess::ensureTile(*tileset, west);
        const Vec3 center = TileBoundsMetrics::tileBoundsCenter(westTile->bounds);
        Camera camera;
        camera.lookAt(center + center.normalized() * 300000.0 /*300 km*/,
                      center, Vec3::unitZ());
        FrameState fs;
        fs.frameId = frameId;
        fs.camera = &camera;
        fs.viewportWidthPixels = 800;
        fs.viewportHeightPixels = 800;
        fs.selectorViews.push_back(makeSelectorView(camera, 800, 800));
        tileset->update(fs);
    }

    // Count materialized tiles across the shallow pyramid z0..maxZ. Used only
    // for pinned-vs-unpinned *comparison* (never an exact-count assertion), so
    // it deliberately does not depend on the internal preload-depth constant:
    // preload covers z0..2 today, and any wider policy still materializes this
    // subset when pinned, keeping the comparison valid.
    int countMaterialized(int maxZ) {
        int n = 0;
        for (int z = 0; z <= maxZ; ++z) {
            for (int x = 0; x < enumScheme->tileCountX(z); ++x) {
                for (int y = 0; y < enumScheme->tileCountY(z); ++y) {
                    if (TilesetTestAccess::findTile(
                            *tileset, TileKey{enumScheme->id(), z, x, y})) {
                        ++n;
                    }
                }
            }
        }
        return n;
    }
};

// Representative tiles in the far EAST hemisphere at z1 and z2. None are roots
// (roots are z0 and always materialized), and all sit far outside a 300 km view
// over the west — so each exists only if base-coverage preload seeded it
// view-independently. If the far corner of the globe is covered, seeding is
// genuinely global.
const std::vector<TileKey> kFarEastProbes = {
    {"Geographic-TMS", 1, 3, 0},
    {"Geographic-TMS", 1, 3, 1},
    {"Geographic-TMS", 2, 7, 1},
    {"Geographic-TMS", 2, 6, 3},
};

} // namespace

// ── 柱② 正向:pinned 时,单帧 update() 与视野无关地种下全球粗底 ────────────────
TEST(ZoomOutBaseCoverage, PinnedSeedsGlobalBaseCoverageViewIndependently) {
    Fixture fx(/*pinBaseCoverage=*/true);
    fx.narrowLowFrame(1);

    // Far-east coarse tiles the narrow west view never touched are all present —
    // seeding reached the opposite hemisphere. (The Preload *group* alone is not
    // a valid signal here: ordinary ancestor/sibling preload also uses it; the
    // view-independent reach to the far hemisphere is base-coverage's signature.)
    for (const TileKey& k : kFarEastProbes) {
        EXPECT_NE(TilesetTestAccess::findTile(*fx.tileset, k), nullptr)
            << "far tile z=" << k.z << " x=" << k.x << " y=" << k.y
            << " must be seeded by preload, not by the camera view";
    }
}

// ── 柱② 证伪(内建"关掉修复会响",不靠截图):unpinned 时视野外粗底不被种入 ──────
TEST(ZoomOutBaseCoverage, UnpinnedDoesNotSeedOutOfViewBaseCoverage) {
    Fixture unpinned(/*pinBaseCoverage=*/false);
    unpinned.narrowLowFrame(1);

    // Every far-east probe is absent: nothing loaded it, so on a later zoom-out
    // it would have no coverage — this is exactly the 漏底 the pin fixes.
    for (const TileKey& k : kFarEastProbes) {
        EXPECT_EQ(TilesetTestAccess::findTile(*unpinned.tileset, k), nullptr)
            << "far tile z=" << k.z << " x=" << k.x << " y=" << k.y
            << " must NOT exist without preload";
    }

    // And pinned strictly out-materializes unpinned over the same shallow
    // pyramid — proving the pinned test asserts a real, discriminating gap
    // rather than something that would hold either way.
    Fixture pinned(/*pinBaseCoverage=*/true);
    pinned.narrowLowFrame(1);
    EXPECT_GT(pinned.countMaterialized(2), unpinned.countMaterialized(2));
}

// ── 柱② 调度:底图种子必须在**非饥饿**优先级(Normal),不在会垫底饿死的 Preload ──
// Preload 严格垫底 + 扁平预算按最高优先级先发 ⇒ 视野内 Normal 恒久饿死底图种子,
// 冷会话地板要几十秒才落地(真机 ~40s 残余 notex 成因)。种在 Normal 队首后只让位
// Urgent。用极小的每帧加载预算(=1)把绝大多数种子留在队列里,直接读它们的 group。
TEST(ZoomOutBaseCoverage, BaseCoverageSeededAtNonStarvablePriority) {
    Fixture fx(/*pinBaseCoverage=*/true, /*maxSimultaneousLoads=*/1);
    fx.narrowLowFrame(1);

    // Far-east z1/z2 tiles only exist via base-coverage seeding (§ above); with a
    // 1-load budget nearly all stay retained in the queue, so we can read the
    // group they were seeded at.
    int inspected = 0;
    for (const TileKey& k : kFarEastProbes) {
        const int group = TilesetTestAccess::queuedGroupOf(*fx.tileset, k);
        if (group < 0) continue;  // dispatched this frame, not retained
        ++inspected;
        EXPECT_EQ(group, static_cast<int>(TileLoadPriorityGroup::Normal))
            << "far tile z=" << k.z << " x=" << k.x << " y=" << k.y
            << " must be seeded at Normal (front), not a starvable tier";
        EXPECT_NE(group, static_cast<int>(TileLoadPriorityGroup::Preload));
    }
    EXPECT_GT(inspected, 0)
        << "expected retained base-coverage seeds to inspect under a 1-load budget";
}

// ============================================================================
// T2(柱③ 影像小泵):泵为从未访问的粗瓦片建立 base 影像 mapping 并发起加载。
// 需要真实内容管线 → MockRenderDevice(无真 GPU)+ base-imagery overlay。
// ============================================================================
namespace {

std::unique_ptr<GltfModel> makeTerrainModel(const Rectangle& rect) {
    auto model = std::make_unique<GltfModel>();
    GltfPrimitive primitive;
    primitive.vertices.resize(3);
    primitive.vertices[0].positionEcef = Vec3(0.0, 0.0, 0.0);
    primitive.vertices[1].positionEcef = Vec3(1.0, 0.0, 0.0);
    primitive.vertices[2].positionEcef = Vec3(0.0, 1.0, 0.0);
    for (SurfaceVertex& v : primitive.vertices) v.normalEcef = Vec3::unitZ();
    primitive.vertices[0].uv = {0.0f, 0.0f};
    primitive.vertices[1].uv = {1.0f, 0.0f};
    primitive.vertices[2].uv = {0.0f, 1.0f};
    primitive.indices = {0, 1, 2};
    primitive.runtime.nodeIndex = 0;
    primitive.runtime.baseVertices = primitive.vertices;
    primitive.runtime.hasNormals = true;
    model->primitives.push_back(std::move(primitive));
    // The rectangle is what lets a Done terrain tile establish its raster
    // mapping — without it the tile stalls at ContentLoading and the pump's
    // canPrepareRasterOverlays() gate never opens.
    model->rasterOverlayDetails.setGeographicRectangle(rect);
    return model;
}

// Serves real terrain (with per-key rectangle) synchronously on request, so
// update()'s load+upload pipeline drives tiles to Done via MockRenderDevice.
class ServingTerrainProvider final : public TilesetContentProvider {
public:
    ServingTerrainProvider(std::vector<TileKey> roots, int maxZoom)
        : roots_(std::move(roots)),
          maxZoom_(maxZoom),
          scheme_(TileScheme::createGeographicTMS()) {}
    std::string id() const override { return "serving-terrain"; }
    bool supportsTile(const TileKey& key) const override {
        return key.z <= maxZoom_;
    }
    std::vector<TileKey> rootTiles() const override { return roots_; }
    bool providesTerrainQuadtree() const override { return true; }
    TileAvailabilityState availabilityState(const TileKey& key) const override {
        return key.z <= maxZoom_ ? TileAvailabilityState::Available
                                 : TileAvailabilityState::NotAvailable;
    }
    bool isTerrainAvailabilityBoundaryLevel(int) const override { return false; }
    void requestTileContent(
        const TileKey& key,
        CancellationToken,
        ContentCallback callback,
        HttpRequestPriority = HttpRequestPriority::Normal) override {
        callback(key,
                 TileContentLoadResult::renderTerrain(
                     makeTerrainModel(scheme_->tileToRectangle(key))));
    }
    TileContentLoadResult decodeContent(const uint8_t*, size_t) override {
        return TileContentLoadResult::failed();
    }
private:
    std::vector<TileKey> roots_;
    int maxZoom_;
    std::unique_ptr<TileScheme> scheme_;
};

RasterOverlay::Options overlayOptions(bool pin) {
    RasterOverlay::Options o;
    o.pinBaseCoverage = pin;
    return o;
}

struct ImageryFixture {
    const TileKey west{"Geographic-TMS", 0, 0, 0};
    const TileKey east{"Geographic-TMS", 0, 1, 0};  // never viewed at low alt

    earth_engine::testing::MockRenderDevice device;
    RasterOverlay overlay;
    ActivatedRasterOverlay activated;
    std::vector<ActivatedRasterOverlay*> overlays;
    std::unique_ptr<Tileset> tileset;

    explicit ImageryFixture(bool pin)
        : overlay(std::make_unique<DebugImageryProvider>(),
                  TileScheme::createXYZWebMercator(),
                  overlayOptions(pin)),
          activated(overlay),
          overlays{&activated} {
        activated.ensureTileProvider(&device);
        TilesetOptions options;
        options.pinBaseCoverage = pin;
        auto provider = std::make_unique<ServingTerrainProvider>(
            std::vector<TileKey>{west, east}, /*maxZoom=*/4);
        tileset = std::make_unique<Tileset>(
            TileScheme::createGeographicTMS(), overlays, &device, options,
            std::move(provider));
    }

    // Stare straight down at the WEST root for `frames` frames at 300 km — the
    // east root is never in the frustum, so any base imagery in flight for it
    // was put there by the pump, not the render-set prefetch.
    void dwellLowOverWest(int frames) {
        TilesetTile* westTile = TilesetTestAccess::ensureTile(*tileset, west);
        const Vec3 c = TileBoundsMetrics::tileBoundsCenter(westTile->bounds);
        for (int f = 1; f <= frames; ++f) {
            Camera cam;
            cam.lookAt(c + c.normalized() * 300000.0, c, Vec3::unitZ());
            FrameState fs;
            fs.frameId = static_cast<uint64_t>(f);
            fs.camera = &cam;
            fs.viewportWidthPixels = 800;
            fs.viewportHeightPixels = 800;
            fs.selectorViews.push_back(makeSelectorView(cam, 800, 800));
            tileset->update(fs);
        }
    }

    const RasterMappedToTilesetTile* baseImageryMapping(const TileKey& key) {
        TilesetTile* t = TilesetTestAccess::findTile(*tileset, key);
        return t ? t->rasterOverlayState.mappingAt(0) : nullptr;
    }
};

} // namespace

// ── 柱③ 正向:pinned 时,泵为从未访问的 east 根建立 base 影像并发起加载 ────────
TEST(ZoomOutBaseCoverage, PinnedPumpDrivesNeverViewedBaseImagery) {
    ImageryFixture fx(/*pinBaseCoverage=*/true);
    fx.dwellLowOverWest(40);

    // The east root was never in the frustum, yet its base imagery mapping is
    // established and an imagery tile is being loaded — only the pump reaches
    // out-of-view tiles (render-set prefetch never would).
    const RasterMappedToTilesetTile* eastMap = fx.baseImageryMapping(fx.east);
    ASSERT_NE(eastMap, nullptr)
        << "pump must establish never-viewed east's base imagery mapping";
    EXPECT_NE(eastMap->getLoadingTile(), nullptr)
        << "pump must initiate the never-viewed east's base imagery load";
    // NOTE: we stop at "load initiated". Promotion to a GPU-ready texture is an
    // engine-generic upload stage with no mock path in this harness (all raster
    // tests hand-set textures), and is not this fix's novelty.
}

// ── 柱③ 证伪:unpinned 时,视野外的 east 根没有任何 base 影像在途 ──────────────
// (进一步隔离"是泵、不是种子"已在源码侧手工验证:只把 runBaseCoveragePreload 的
//  影像小泵段短路、保留种子段,pinned 的 east 变成已种(loadState 前进)但
//  baseImageryMapping 归 nullptr —— 断言随之变红。)
TEST(ZoomOutBaseCoverage, UnpinnedLeavesNeverViewedImageryUntouched) {
    ImageryFixture fx(/*pinBaseCoverage=*/false);
    fx.dwellLowOverWest(40);

    EXPECT_EQ(fx.baseImageryMapping(fx.east), nullptr)
        << "without the pump, a never-viewed tile has no base imagery in flight";
}
