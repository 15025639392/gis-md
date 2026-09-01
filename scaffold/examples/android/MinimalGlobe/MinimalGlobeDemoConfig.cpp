#include "MinimalGlobeDemoConfig.h"

#include <algorithm>

namespace earth_engine::minimal_globe_demo {

AmapWorkerBudget chooseAmapWorkerBudget(int cpuCores,
                                        int64_t totalMemoryBytes) {
    const size_t cores = static_cast<size_t>(std::max(1, cpuCores));
    const size_t usable = cores > 2 ? cores - 2 : 1;
    const int64_t gib = 1024LL * 1024LL * 1024LL;
    if (usable <= 2) return {1, 1, 1};

    // Unknown memory is deliberately conservative: keep one decode lane and
    // at most two tessellation lanes, while still separating the queues.
    if (totalMemoryBytes <= 0 || totalMemoryBytes <= 6 * gib) {
        const size_t tess =
            std::min<size_t>(2, usable > 2 ? usable - 2 : 1);
        return {1, 1, std::max<size_t>(1, tess)};
    }

    if (totalMemoryBytes <= 12 * gib) {
        const size_t decode = std::min<size_t>(2, usable);
        const size_t tessBudget =
            usable > decode + 1 ? usable - decode - 1 : 1;
        return {decode, 1,
                std::max<size_t>(1, std::min<size_t>(3, tessBudget))};
    }

    const size_t decode = std::min<size_t>(2, usable);
    const size_t tessBudget =
        usable > decode + 1 ? usable - decode - 1 : 1;
    return {decode, 1,
            std::max<size_t>(1, std::min<size_t>(4, tessBudget))};
}

EarthSceneConfig makeDefaultDemoSceneConfig() {
    EarthSceneConfig config;
    config.initialCamera = {
        kMeasureLongitudeDegrees,
        kMeasureLatitudeDegrees,
        kMeasureHorizonView ? kMeasureHorizonHeightMeters
                            : kMeasureHeightMeters,
        kMeasureHorizonView ? kMeasureHorizonElevationDegrees
                            : kMeasureObliqueElevationDegrees,
    };
    // 地平线日落 A/B:相机看向日落太阳方位(否则默认 0=正北,与旧行为等价)。
    config.initialCamera.obliqueAzimuthDegrees =
        (kMeasureHorizonView && kMeasureHorizonSunset)
            ? kMeasureHorizonAzimuthDegrees
            : 0.0;
    // 测量台冻结相机：初始位姿设定后 update() 空转，far 位姿也可复现（见 header）。
    config.initialCamera.freezeCamera = kMeasureFreezeCamera;
    // 测量台脚本化平移（§14.1② live 换页净测，见 header）：确定性受控运动量 ghost。
    config.initialCamera.scriptedPan = kMeasureScriptedPan;
    config.initialCamera.scriptedPanStartFrame = kMeasureScriptedPanStartFrame;
    config.initialCamera.scriptedPanFrames = kMeasureScriptedPanFrames;
    config.initialCamera.scriptedPanYawPerFrameRad =
        kMeasureScriptedPanYawPerFrameDegrees * 0.017453292519943295;  // deg→rad

    config.tileset = {
        4.0,
        2.0,
    };
    // 北极星生产主路径:纹理/几何解耦默认开(几何 cap 在 DEM native z12,影像不再
    // 驱动几何 refine → 无捏造 z13+ notReady 空洞 = 收底部露天空)。配下方
    // terrainPageStore 在 capped z12 面贴 z14+ 屏幕界定高清影像(近景仍 crisp)。
    // 二者是生产配对(§15.3⑤)。A/B 测耦合基线时把此处改 false。
    config.tileset.decoupleImageryFromGeometry = true;
    // Android demo budget: keep visible detail unchanged, but do not retain the
    // desktop/cesium-native 512MB off-screen tile cache on a phone.
    config.tileset.maximumCachedBytes = 192LL * 1024 * 1024;
    // 运动期跳过快速划走的瓦片网络请求(cesium-js cullRequestsWhileMoving)。
    // 拖动/缩放中减少瞬时加载洪泛,相机停下恢复正常加载。
    config.tileset.cullRequestsWhileMoving = true;
    // 地形 fill 代理:根瓦片加载期间,先把已到的影像贴到椭球代理,真实地形网格
    // 到达后再替换,避免加载窗口只剩天空。
    // 纯矢量 Amap 模式明确关闭 raster imagery 时，不要再创建 terrain
    // fill-proxy 的空 raster 页。该 proxy 在无 source 时仍会提交
    // RasterOverlayTileProvider fallback 纹理，造成远景灰色碎纹并占用
    // imagery cache；有真实地形/影像时才保留它作为加载期兜底。
    config.tileset.enableTerrainFillProxy = false;
    // 接边错位诊断探针默认关(常开每帧 ~4ms=selPlan 大头,无缝已收官)。
    config.tileset.seamEdgeMismatchProbe = kEnableSeamEdgeMismatchProbe;
    // LOD geomorph:距离连续 geomorph 已启用(P2 引擎 + P3 skirt 之上)。morph 进度
    // 纯由本瓦片 SSE 驱动(finalizer,gate 在 maxSSE>0)。子瓦片从 morph=0
    // (coarse-self≈父面,worker 烘焙 heightDelta,规则栅格 in-tile 自降采样,根治旧
    // 变体 A「浮上来」)平滑 morph 到 morph=1(真实细节),随相机连续移动推进,消除
    // 硬 pop。相邻瓦片不同 morph 进度间的边缝由 skirt 遮盖。时序 cross-fade 通路
    // 已于 2026-08-07 整链删除(被 geomorph 顶替)。

    // Official AMap owns the complete basemap color contract. A generic
    // atmosphere pass would recolor distant surfaces outside that contract.
    config.aerialFog = false;
    // 北极星 Phase 2b 虚拟纹理 PoC(默认关,见 header)。
    config.virtualTexturePoc = kMeasureVirtualTexturePoC;
    // 北极星 Phase 2b B 方案逐瓦片合成 PoC(默认关,见 header)。
    config.tileCompositeBakePoc = kMeasureTileCompositeBakePoC;
    // 北极星 Phase 2b 合成方案 门① 原型(默认关,见 header)。
    config.vtIndirectionSamplePoc = kMeasureVtIndirectionSamplePoC;
    // Pure-vector AMap owns the complete basemap contract. Do not instantiate
    // the raster/page-store lifecycle when there is no raster source.
    config.terrainPageStore = false;
    // GPU 逐区间计时(见 header)。默认关:每帧 ~8 个 timer query,是测量台不是
    // 生产开销。要查 GPU 时间去哪了就把 header 里那个常量翻 true。
    config.gpuPassTiming = kMeasureGpuPassTiming;
    config.blackFrameProbe = kBlackFrameProbe;
    config.frameGating = kEnableFrameGating;
    config.gpuHeightBake = kEnableGpuHeightBake;
    config.shadowVerifyIdle = kShadowVerifyIdle;
    config.fixedSimulationJulianDate = kFixedSimulationJulianDate;
    if (kMeasureHorizonView && kMeasureHorizonSunset) {
        // 日落 A/B:时钟设到太阳贴地平线的低角(覆盖地形验收的近顶太阳)。
        config.fixedSimulationJulianDate =
            2461188.75 + kMeasureHorizonSunsetJulianOffset;
    }
    return config;
}

} // namespace earth_engine::minimal_globe_demo
