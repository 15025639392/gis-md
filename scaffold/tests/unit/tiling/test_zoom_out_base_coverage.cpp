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
//                     每帧单独推进其 base 影像就绪。这一柱是 base-IMAGERY(notex)
//                     就绪,依赖 overlay + 跨帧地形/影像耦合,不在此 host 锁的
//                     范围(见 TilesetUpdateFrameRuntime.cpp runBaseCoveragePreload
//                     顶部注释与 demo BlackFrameProbe)。
//
// 本文件锁柱②的可判据不变量:种子是否**与视野无关地**覆盖全球 z0..2 —— 这正是
// "捏到从未加载经度不露底"的必要条件。两个方向都测(pinned 种满 / unpinned 不种),
// 后者即"故意关掉修复,断言会响"的内建证伪 —— 不靠截图。
//
// GPU-free / device-free:纯选择+加载队列逻辑,不渲染、不读像素。

#include <gtest/gtest.h>

#include "earth_engine/content/GltfContentProvider.h"
#include "earth_engine/core/geodesy/Ellipsoid.h"
#include "earth_engine/core/math/Vec3.h"
#include "earth_engine/scene/Camera.h"
#include "earth_engine/scene/FrameState.h"
#include "earth_engine/tiling/TileBaseCoveragePin.h"
#include "earth_engine/tiling/TileBoundsMetrics.h"
#include "earth_engine/tiling/TileScheme.h"
#include "earth_engine/tiling/Tileset.h"

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

    explicit Fixture(bool pinBaseCoverage) {
        TilesetOptions options;
        options.pinBaseCoverage = pinBaseCoverage;
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
