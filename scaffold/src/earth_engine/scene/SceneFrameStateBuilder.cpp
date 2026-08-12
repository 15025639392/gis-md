#include "SceneFrameStateBuilder.h"

#include "Camera.h"
#include "SceneSelectorViewBuilder.h"

#include "../core/geodesy/Ellipsoid.h"
#include "../debug/PerfTimer.h"
#include "../environment/SkyGradient.h"
#include "../environment/SunDirection.h"
#include "../environment/TimeController.h"

namespace earth_engine {
namespace {

constexpr double kInteractionFocusTtlSeconds = 2.5;

// ── 日落地表着色档位(编译期,项目 constexpr 配置惯例;改此处即换全局调性)──
// 参数覆盖真实黄金时刻航拍谱系(Pexels 6942523/3559235=自然、12897133=金光)。
// 要暴露到 SDK EarthSceneConfig 运行时可配是另一步(穿 coordinator 链路)。
struct TerrainSunsetTint {
    float warmth;           // 受光面暖度(sunTint 往 sunsetTint 偏多少,0..1)
    float noonTint[3];      // 白天受光色(微暖白);sunLow=0 时 = 零回归基准
    float sunsetTint[3];    // 日落受光目标暖色
    float shadowAmbient[3]; // 阴影面暖补光(日落满档;乘 baseRgb 相加)
};
// 「自然」档(定):暖橙适度、保地物本色。参考档:
//   金光 warmth=0.9 / sunsetTint{1.0,0.55,0.28} / shadowAmbient×1.4;
//   物理 warmth=0.5 / shadowAmbient 偏冷{0.20,0.18,0.22}。
constexpr TerrainSunsetTint kSunsetTintNatural = {
    0.75f,
    {1.05f, 1.00f, 0.91f},
    {1.00f, 0.62f, 0.38f},
    {0.35f, 0.20f, 0.12f}};

void updateInteractionFocus(
    FrameState& frameState,
    bool hasInteractionFocus,
    const Vec3& interactionFocusDirection,
    double interactionFocusTimeSeconds) {
    frameState.hasInteractionFocus =
        hasInteractionFocus &&
        interactionFocusTimeSeconds >= 0.0 &&
        frameState.timeSeconds - interactionFocusTimeSeconds <=
            kInteractionFocusTtlSeconds;
    frameState.interactionFocusDirection = frameState.hasInteractionFocus
        ? interactionFocusDirection
        : Vec3::zero();
}

double updateEnvironment(const SceneFrameStateBuildInput& input) {
    if (!input.timeController || !input.skyGradient || !input.camera) {
        return 0.0;
    }

    const double startMs = perf::nowMs();
    FrameState& frameState = input.frameState;
    Vec3 sunDir = SunDirection::compute(input.timeController->julianDate());
    double camAlt = input.camera->getHeight();
    Vec3 localUp =
        Ellipsoid::WGS84().geodeticSurfaceNormal(input.camera->position());
    input.skyGradient->update(sunDir, localUp, camAlt);
    frameState.lightDir = {
        static_cast<float>(sunDir.x()),
        static_cast<float>(sunDir.y()),
        static_cast<float>(sunDir.z())};
    auto& hc = input.skyGradient->horizonColor();
    frameState.clearR = hc[0];
    frameState.clearG = hc[1];
    frameState.clearB = hc[2];
    auto& ac = input.skyGradient->ambientColor();
    frameState.ambient = {ac[0], ac[1], ac[2]};

    // 日落地表暖化:太阳贴地平线(sunElev→0)时受光色温转暖橙。sunLow 曲线与天空
    // pass 逐字一致(smoothstep 0→0.30,见 AtmosphereSkyColorGLSL.h)。⚠️ 两处各
    // 推同一曲线:改暖度曲线须同步天空侧,否则天地暖度错位(共享口径,瘦版让步)。
    double sunElev = sunDir.x() * localUp.x() + sunDir.y() * localUp.y() +
                     sunDir.z() * localUp.z();
    double e = sunElev > 0.0 ? sunElev : 0.0;
    double t = e / 0.30;
    t = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
    double sunLow = 1.0 - t * t * (3.0 - 2.0 * t);
    // 日落地表着色:受光面 sunTint(暖度=warmth) + 阴影面暖补光(=shadowAmbient×
    // sunLow)。档位见 kSunsetTintNatural。白天 sunLow=0 → sunTint=noonTint、
    // ambient=0 = 现状零回归。
    // 色基准=编译期「自然」档;强度(warmth/shadowScale)运行时可配,来自
    // EarthSceneConfig → SkyGradient。config 默认 0.75/1.0 = 档位基准 → 逐字等价。
    const TerrainSunsetTint& st = kSunsetTintNatural;
    float warmth = input.skyGradient->sunsetWarmth();
    float shadowScale = input.skyGradient->sunsetShadowScale();
    float f = static_cast<float>(sunLow) * warmth;
    frameState.sunTint = {
        st.noonTint[0] + (st.sunsetTint[0] - st.noonTint[0]) * f,
        st.noonTint[1] + (st.sunsetTint[1] - st.noonTint[1]) * f,
        st.noonTint[2] + (st.sunsetTint[2] - st.noonTint[2]) * f};
    float sl = static_cast<float>(sunLow);
    frameState.terrainSunAmbient = {st.shadowAmbient[0] * sl * shadowScale,
                                    st.shadowAmbient[1] * sl * shadowScale,
                                    st.shadowAmbient[2] * sl * shadowScale};
    return perf::nowMs() - startMs;
}

} // namespace

SceneFrameStateBuildResult SceneFrameStateBuilder::build(
    const SceneFrameStateBuildInput& input) {
    FrameState& frameState = input.frameState;
    frameState.frameId = input.frameId;
    frameState.timeSeconds = input.timeSeconds;
    frameState.deltaSeconds = input.deltaSeconds;
    frameState.camera = input.camera;

    SceneSelectorViewBuilder::populate(
        frameState,
        SceneSelectorViewBuildInput{
            input.camera,
            frameState.viewportWidthPixels,
            frameState.viewportHeightPixels,
            input.hasSelectorViewOverride,
            input.selectorViewOverride});
    updateInteractionFocus(
        frameState,
        input.hasInteractionFocus,
        input.interactionFocusDirection,
        input.interactionFocusTimeSeconds);
    return SceneFrameStateBuildResult{
        updateEnvironment(input)};
}

} // namespace earth_engine
