#include <gtest/gtest.h>

#include "earth_engine/providers/DebugImageryProvider.h"
#include "earth_engine/providers/RasterOverlayTile.h"
#include "earth_engine/providers/RasterOverlayTileProvider.h"
#include "earth_engine/content/GltfModel.h"
#include "earth_engine/core/math/Mat4.h"
#include "earth_engine/core/resources/FrameResourceBudget.h"
#include "earth_engine/layers/ActivatedRasterOverlay.h"
#include "earth_engine/layers/RasterOverlay.h"
#include "earth_engine/renderer/IPrepareRendererResources.h"
#include "earth_engine/tiling/DirectRasterMapping.h"
#include "earth_engine/tiling/RasterOverlayProjection.h"
#include "earth_engine/tiling/TileRasterOverlayPrefetcher.h"
#include "earth_engine/tiling/TileScheme.h"
#include "earth_engine/tiling/TilesetTile.h"
#include "../../helpers/MockRenderDevice.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <numeric>
#include <vector>

using namespace earth_engine;

// =============================================================================
// I-P3 量化:空洞瓦每帧重走完整 update()。
//
// 机制(2026-08-22 静态核实):空合成瓦(markLoadedWithoutTexture → Loaded、
// 无纹理、rendererResources==nullptr)被 Step 3 提升为 ready 后,Step 1 早退
// 判定「ready 非 Failed 且 rendererResources==nullptr」每帧把 state 踢回
// Unattached → hasStableUpdateState() 恒 false → 每帧走完整 update() 体。
// 稳定瓦(纹理已附着)在 Step 1 直接早退。
//
// 本测试用生产 provider + mapping 复现两种状态,测每瓦每帧 update() 成本差,
// 供「洞区通常少,但每帧成本是否值得优化」裁决。
// =============================================================================

namespace {

using Clock = std::chrono::steady_clock;

// 生产里 update() 总带 prep renderer(attach 通知);基准用 no-op mock 保持
// 路径一致。空洞瓦 attach 因无纹理早退,稳定瓦 attach 后进入 Attached。
class NoopPrepareRendererResources final
    : public IPrepareRendererResources {
public:
    void attachRasterInMainThread(
        const TileKey&,
        int32_t,
        std::shared_ptr<const RasterOverlayTile>,
        Texture*,
        float,
        float,
        float,
        float) override {}
    void detachRasterInMainThread(const TileKey&, int32_t) noexcept override {}
};

double elapsedMs(Clock::time_point start, Clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}

RasterOverlayDetails providerDetails(const TileScheme& scheme,
                                     const Rectangle& bounds) {
    RasterOverlayDetails details;
    details.rasterOverlayProjections = {
        RasterOverlayProjection::WebMercator};
    details.rasterOverlayRectangles = {
        projectWorldRectangleForRasterOverlay(
            bounds, RasterOverlayProjection::WebMercator)};
    details.boundingRegion = {bounds, 0.0, 0.0};
    return details;
}

// 把 mapping 推进到目标状态并返回每帧 update() 均值(ms)。
double measureUpdatePerFrame(DirectRasterMapping& mapped,
                             const TileKey& key,
                             const RasterOverlayDetails& details,
                             RasterOverlayTileProvider& provider,
                             IPrepareRendererResources* prepRenderer,
                             int warmup,
                             int samples) {
    std::vector<RasterOverlayProjection> missing;
    for (int i = 0; i < warmup; ++i) {
        mapped.update(
            key, details, 512.0, 512.0, provider, prepRenderer, missing,
            nullptr, 0, true);
    }
    std::vector<double> samplesMs;
    samplesMs.reserve(samples);
    for (int i = 0; i < samples; ++i) {
        const Clock::time_point start = Clock::now();
        mapped.update(
            key, details, 512.0, 512.0, provider, prepRenderer, missing,
            nullptr, 0, true);
        samplesMs.push_back(elapsedMs(start, Clock::now()));
    }
    std::sort(samplesMs.begin(), samplesMs.end());
    return std::accumulate(samplesMs.begin(), samplesMs.end(), 0.0) /
           static_cast<double>(samplesMs.size());
}

}  // namespace

TEST(RasterHoleUpdateCost, HoleTilePerFrameUpdateVsStable) {
    DebugImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *scheme, nullptr);
    NoopPrepareRendererResources prep;

    // ── 空洞 mapping(空合成瓦:无纹理,每帧被踢回 Unattached)──
    const TileKey holeKey{scheme->id(), 1, 1, 1};
    const Rectangle holeBounds = scheme->tileToRectangle(holeKey);
    TilesetTile holeTile(holeKey, holeBounds);
    DirectRasterMapping& hole =
        holeTile.rasterOverlayState.ensureMapping(0);
    const RasterOverlayDetails holeDetails =
        providerDetails(*scheme, holeBounds);
    std::vector<RasterOverlayProjection> holeMissing;
    hole.update(
        holeKey, holeDetails, 512.0, 512.0, provider, &prep, holeMissing,
        nullptr, 0, true);
    RasterOverlayTile* holeLoading = hole.getLoadingTile();
    ASSERT_NE(nullptr, holeLoading);
    holeLoading->markLoadedWithoutTexture();  // 空合成
    hole.update(
        holeKey, holeDetails, 512.0, 512.0, provider, &prep, holeMissing,
        nullptr, 0, true);
    EXPECT_FALSE(hole.hasStableUpdateState())
        << "空洞瓦应处于非稳定态(每帧重走完整 update)";

    // ── 稳定 mapping(纹理已附着,Step 1 早退)──
    const TileKey stableKey{scheme->id(), 1, 0, 1};
    const Rectangle stableBounds = scheme->tileToRectangle(stableKey);
    TilesetTile stableTile(stableKey, stableBounds);
    DirectRasterMapping& stable =
        stableTile.rasterOverlayState.ensureMapping(0);
    const RasterOverlayDetails stableDetails =
        providerDetails(*scheme, stableBounds);
    std::vector<RasterOverlayProjection> stableMissing;
    stable.update(
        stableKey, stableDetails, 512.0, 512.0, provider, &prep,
        stableMissing, nullptr, 0, true);
    RasterOverlayTile* stableLoading = stable.getLoadingTile();
    ASSERT_NE(nullptr, stableLoading);
    stableLoading->setTexture(
        std::make_unique<earth_engine::testing::DummyTexture>(4, 4));
    stable.update(
        stableKey, stableDetails, 512.0, 512.0, provider, &prep,
        stableMissing, nullptr, 0, true);
    EXPECT_TRUE(stable.hasStableUpdateState())
        << "稳定瓦应处于稳定态(Step 1 早退)";

    const double holeMs =
        measureUpdatePerFrame(
            hole, holeKey, holeDetails, provider, &prep, 200, 5000);
    const double stableMs = measureUpdatePerFrame(
        stable, stableKey, stableDetails, provider, &prep, 200, 5000);
    RecordProperty("hole_update_per_frame_ms", holeMs);
    RecordProperty("stable_update_per_frame_ms", stableMs);
    std::printf(
        "[I-P3] update() 每瓦每帧:空洞 %.5f ms vs 稳定 %.5f ms(差值 %.5f ms)\n",
        holeMs, stableMs, holeMs - stableMs);
    // 方向性 sanity:空洞态不应比稳定态快(测量噪声下 0.5× 保守界)。
    EXPECT_GT(holeMs, stableMs * 0.5)
        << "空洞态 update 竟然比稳定态还快,状态机假设可能错了";
}

// 帧循环真实入口(prefetch)的空洞 vs 稳定每帧成本。
TEST(RasterHoleUpdateCost, HoleTilePerFramePrefetchVsStable) {
    FrameResourceBudgetConfig config;
    config.maxRasterNetworkRequestsPerFrame = 64;
    config.maxRasterNetworkInflight = 64;
    NoopPrepareRendererResources prep;

    auto runFrames = [&](bool holeMode, int warmup, int samples) -> double {
        auto overlay = std::make_unique<RasterOverlay>(
            std::make_unique<DebugImageryProvider>(),
            TileScheme::createXYZWebMercator(),
            RasterOverlay::Options{});
        ActivatedRasterOverlay activated(*overlay);
        RasterOverlayTileProvider* provider =
            activated.ensureTileProvider(nullptr);
        if (!provider) {
            ADD_FAILURE() << "provider 未就绪";
            return 0.0;
        }
        const TileKey key{"XYZ-WebMercator", 1, 1, 1};
        const Rectangle bounds =
            overlay->getTileScheme().tileToRectangle(key);
        TilesetTile tile(key, bounds);
        auto model = std::make_unique<GltfModel>();
        model->rasterOverlayDetails.rasterOverlayProjections.push_back(
            RasterOverlayProjection::WebMercator);
        model->rasterOverlayDetails.rasterOverlayRectangles.push_back(
            projectWorldRectangleForRasterOverlay(
                bounds, RasterOverlayProjection::WebMercator));
        tile.content.renderContent.prepareGltfContent(
            std::move(model), Mat4::identity());
        tile.content.renderContent.addGltfPrimitiveResource(
            GltfPrimitiveRenderResources{});
        tile.content.loadState = TileLoadState::Done;
        tile.content.contentKind = TileContentKind::Render;
        tile.geometricError = 100.0;

        std::vector<ActivatedRasterOverlay*> overlays{&activated};
        FrameResourceBudget budget;
        budget.beginFrame(1, config);
        TileRasterOverlayPrefetcher::prefetch(
            tile, overlays, {0}, nullptr, 16.0, budget, &prep, 1);
        DirectRasterMapping* mapped =
            tile.rasterOverlayState.mappingAt(0);
        if (!mapped) {
            ADD_FAILURE() << "mapping 未创建";
            return 0.0;
        }
        RasterOverlayTile* loading = mapped->getLoadingTile();
        if (!loading) {
            ADD_FAILURE() << "loading tile 未创建";
            return 0.0;
        }
        if (holeMode) {
            loading->markLoadedWithoutTexture();
        } else {
            loading->setTexture(
                std::make_unique<earth_engine::testing::DummyTexture>(4, 4));
        }
        budget.beginFrame(2, config);
        TileRasterOverlayPrefetcher::prefetch(
            tile, overlays, {0}, nullptr, 16.0, budget, &prep, 2);
        if (holeMode) {
            EXPECT_FALSE(mapped->hasStableUpdateState());
        } else {
            EXPECT_TRUE(mapped->hasStableUpdateState());
        }

        std::vector<double> samplesMs;
        samplesMs.reserve(samples);
        for (int i = 0; i < warmup + samples; ++i) {
            budget.beginFrame(
                static_cast<uint64_t>(3 + i), config);
            const Clock::time_point start = Clock::now();
            TileRasterOverlayPrefetcher::prefetch(
                tile, overlays, {0}, nullptr, 16.0, budget, &prep,
                static_cast<uint64_t>(3 + i));
            if (i >= warmup) {
                samplesMs.push_back(elapsedMs(start, Clock::now()));
            }
        }
        std::sort(samplesMs.begin(), samplesMs.end());
        return std::accumulate(samplesMs.begin(), samplesMs.end(), 0.0) /
               static_cast<double>(samplesMs.size());
    };

    const double holeMs = runFrames(/*holeMode=*/true, 100, 1000);
    const double stableMs = runFrames(/*holeMode=*/false, 100, 1000);
    RecordProperty("hole_prefetch_per_frame_ms", holeMs);
    RecordProperty("stable_prefetch_per_frame_ms", stableMs);
    std::printf(
        "[I-P3] prefetch() 每瓦每帧:空洞 %.5f ms vs 稳定 %.5f ms(差值 %.5f ms)\n",
        holeMs, stableMs, holeMs - stableMs);
    EXPECT_GT(holeMs, stableMs * 0.5)
        << "空洞态 prefetch 竟然不比稳定态慢,状态机假设可能错了";
}
