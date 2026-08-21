#include "SkyGradient.h"
#include "SkyColorModel.h"
#include <glm/gtc/constants.hpp>
#include <cmath>
#include <algorithm>

namespace earth_engine {

namespace {

double clamp01(double x) { return x < 0.0 ? 0.0 : (x > 1.0 ? 1.0 : x); }

/// 与 GPU 大气 pass 相同的 spaceFactor 公式(120km→900km 平滑)。
double skySpaceFactor(double cameraAltitudeMeters) {
    double t = (cameraAltitudeMeters - 120000.0) / 780000.0;
    t = clamp01(t);
    return t * t * (3.0 - 2.0 * t);
}

}  // namespace

SkyGradient::SkyGradient()
    : params_(earthAtmosphereDefaults()) {}

SkyGradient::SkyGradient(const AtmosphereParameters& params)
    : params_(params) {
    params_.validate();
}

void SkyGradient::setParameters(const AtmosphereParameters& params) {
    params_ = params;
    params_.validate();
}

void SkyGradient::update(const Vec3& sunDirECEF,
                         const Vec3& localUpECEF,
                         double cameraAltitudeMeters) {
    const Vec3 sun = sunDirECEF.lengthSquared() > 1e-12
        ? sunDirECEF.normalized()
        : Vec3::unitZ();
    const Vec3 localUp = localUpECEF.lengthSquared() > 1e-12
        ? localUpECEF.normalized()
        : Vec3::unitZ();
    const double sunDotUp = std::clamp(sun.dot(localUp), -1.0, 1.0);
    sunElevation_ = std::asin(sunDotUp);

    // ============================================================
    // 颜色 = 与 GPU 天空/雾同一 computeSkyColor(经验色板,单一事实源)
    // ============================================================
    //
    // L-P1:此前这里是独立的解析散射(Rayleigh+Mie+Ozone+expose)实现,与
    // computeSkyColor 数值漂移(正午 horizon 差 7 倍)→ 清屏色/环境光与
    // 用户看到的天空"不配套"。合并为直接采样 computeSkyColorCpu:
    //   - 天顶色   = 视线沿 localUp
    //   - 地平线色 = 视线朝太阳的水平方向(GPU pass 里地平线带也是朝阳侧最亮)
    //   - 环境光   = 天顶/地平线加权(保留原 0.6/0.4 半球近似)
    // computeSkyColor 本身无夜晚语义(它的 spaceFactor 只随高度),故夜晚
    // 门控保留:太阳在地平线下时乘 nightFactor 压暗,并保留星光下限。
    const double spaceFactor = skySpaceFactor(cameraAltitudeMeters);
    const std::array<double, 3> up = {localUp.x(), localUp.y(), localUp.z()};
    const std::array<double, 3> sunArr = {sun.x(), sun.y(), sun.z()};

    // 天顶方向
    const std::array<double, 3> zenith =
        computeSkyColorCpu(up, up, sunArr, spaceFactor);
    // 地平线方向:朝太阳的水平分量,归一化
    const double sunUpDot = sunArr[0] * up[0] + sunArr[1] * up[1] +
                            sunArr[2] * up[2];
    std::array<double, 3> horizDir = {
        sunArr[0] - up[0] * sunUpDot,
        sunArr[1] - up[1] * sunUpDot,
        sunArr[2] - up[2] * sunUpDot};
    const double horizLen = std::sqrt(horizDir[0] * horizDir[0] +
                                      horizDir[1] * horizDir[1] +
                                      horizDir[2] * horizDir[2]);
    if (horizLen > 1e-9) {
        horizDir[0] /= horizLen;
        horizDir[1] /= horizLen;
        horizDir[2] /= horizLen;
    } else {
        // 太阳恰在天顶/天底:任意水平方向
        horizDir = {1.0, 0.0, 0.0};
        const double hDot =
            horizDir[0] * up[0] + horizDir[1] * up[1] + horizDir[2] * up[2];
        horizDir[0] -= up[0] * hDot;
        horizDir[1] -= up[1] * hDot;
        horizDir[2] -= up[2] * hDot;
        const double hl = std::sqrt(horizDir[0] * horizDir[0] +
                                    horizDir[1] * horizDir[1] +
                                    horizDir[2] * horizDir[2]);
        if (hl > 1e-9) {
            horizDir[0] /= hl;
            horizDir[1] /= hl;
            horizDir[2] /= hl;
        }
    }
    const std::array<double, 3> horizon =
        computeSkyColorCpu(horizDir, up, sunArr, spaceFactor);

    // 夜晚门控:太阳在地平线下按指数衰减压暗(与 SceneRenderPipeline 的
    // SkyBox nightFactor 同式);星光下限保留。
    double nightFactor = 1.0;
    if (sunElevation_ < -0.05) {
        nightFactor = std::clamp(std::exp(sunElevation_ * 8.0), 0.0, 1.0);
    }

    for (int c = 0; c < 3; ++c) {
        zenith_[c] = static_cast<float>(zenith[c] * nightFactor);
        horizon_[c] = static_cast<float>(horizon[c] * nightFactor);
        double amb = (zenith[c] * 0.6 + horizon[c] * 0.4) * 0.35 * nightFactor;
        amb = std::max(amb, sunElevation_ < -0.05 ? 0.002 : 0.005);
        ambient_[c] = static_cast<float>(amb);
    }
    zenith_[3] = 1.0f;
    horizon_[3] = 1.0f;
}

} // namespace earth_engine
