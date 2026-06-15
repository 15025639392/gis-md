#include "SkyGradient.h"
#include <glm/gtc/constants.hpp>
#include <cmath>
#include <algorithm>

namespace earth_engine {

// ============================================================
// 大气物理常量
// ============================================================

namespace {

constexpr double kPi = glm::pi<double>();
constexpr double kInv4Pi = 1.0 / (4.0 * kPi);

/// Rayleigh 相函数：p(θ) = 3/(16π) * (1 + cos²θ)
double rayleighPhase(double cosTheta) {
    return (3.0 / (16.0 * kPi)) * (1.0 + cosTheta * cosTheta);
}

/// Mie 相函数（Henyey-Greenstein）：g = 0.76
/// p(θ) = (1-g²)/(4π) * 1/(1+g²-2g·cosθ)^(3/2)
double miePhase(double cosTheta, double g = 0.76) {
    double g2 = g * g;
    double denom = 1.0 + g2 - 2.0 * g * cosTheta;
    // clamp to avoid singularity
    denom = std::max(denom, 1e-6);
    return kInv4Pi * (1.0 - g2) / (denom * std::sqrt(denom));
}

/// 臭氧密度分布：ρ(h) = max(0, 1 - |h - h_O| / w_O)
double ozoneDensity(double height, double centerHeight, double halfWidth) {
    return std::max(0.0, 1.0 - std::abs(height - centerHeight) / halfWidth);
}

/// 沿指数密度分布路径的光学深度（解析积分）
/// 从起点高度 h0，沿天顶角 θ 的路径，到大气层顶
/// β(h) = β0 * exp(-h / H)
/// τ = ∫ β(h(s)) ds
///
/// 对于垂直路径：τ = β0 * H * (1 - exp(-h_max/H)) ≈ β0 * H（当 h_max ≫ H）
/// 对于斜路径（天顶角 θ）：τ ≈ β0 * H / cos(θ)（平面平行近似，θ < 80°）
double opticalDepthVertical(double beta0, double scaleHeight,
                            double hStart, double hEnd) {
    // ∫_{hStart}^{hEnd} β0 * exp(-h/H) dh = β0 * H * (exp(-hStart/H) - exp(-hEnd/H))
    return beta0 * scaleHeight *
           (std::exp(-hStart / scaleHeight) - std::exp(-hEnd / scaleHeight));
}

double smoothstep(double edge0, double edge1, double x) {
    double t = std::clamp((x - edge0) / (edge1 - edge0), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

float expose(double radiance, double exposure) {
    const double mapped = 1.0 - std::exp(-std::max(radiance, 0.0) * exposure);
    return std::clamp(static_cast<float>(mapped), 0.0f, 1.0f);
}

} // anonymous namespace

// ============================================================
// SkyGradient
// ============================================================

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
    const double daylight = smoothstep(-0.06, 0.04, sunElevation_);

    // ============================================================
    // 物理 Rayleigh + Mie 散射计算
    // ============================================================

    const auto& p = params_;

    // 海平面散射系数（m⁻¹）
    const double betaR[] = {
        p.rayleigh.r * 1e-6 * p.rayleighSeaLevelScattering,
        p.rayleigh.g * 1e-6 * p.rayleighSeaLevelScattering,
        p.rayleigh.b * 1e-6 * p.rayleighSeaLevelScattering
    };
    const double betaM = p.mie.scattering * 1e-6 * p.mieSeaLevelScattering;
    const double betaO[] = {
        p.ozone.r * 1e-6,
        p.ozone.g * 1e-6,
        p.ozone.b * 1e-6
    };

    // 光学深度（从海平面到大气层顶的垂直路径）
    double tauR[3], tauM;
    for (int c = 0; c < 3; ++c) {
        tauR[c] = opticalDepthVertical(betaR[c], p.rayleighScaleHeight,
                                        0.0, p.atmosHeight);
    }
    tauM = opticalDepthVertical(betaM, p.mieScaleHeight, 0.0, p.atmosHeight);

    // 相机高度处的光学深度衰减因子
    const double cameraHeight = std::clamp(cameraAltitudeMeters, 0.0, p.atmosHeight);
    double camHeightRatioR = std::exp(-cameraHeight / p.rayleighScaleHeight);
    double camHeightRatioM = std::exp(-cameraHeight / p.mieScaleHeight);

    // ========================================================
    // 天顶颜色 — 观察方向垂直向上
    // ========================================================
    //
    // viewDir = up (local zenith)
    // cos(scattering angle) = sunDir · viewDir = sin(sunElevation)
    // 因为 viewDir = (0,0,1) in local ENU, sunDir_ENU 的 z 分量 = sin(elevation)
    //
    double cosThetaZenith = sunDotUp;

    double phaseR_Z = rayleighPhase(cosThetaZenith);
    double phaseM_Z = miePhase(cosThetaZenith);

    // 太阳光到散射点的光学深度（太阳在天顶角 π/2-elevation）
    double cosSunZenith = std::max(sunDotUp, 0.02);
    // 太阳光在大气中的路径：从大气层顶到海平面
    // 平面平行近似：τ_sun = τ_vertical / cos(sunZenithAngle)
    double sunPathScale = 1.0 / cosSunZenith;

    for (int c = 0; c < 3; ++c) {
        // 沿视线散射
        double scatteredR = tauR[c] * phaseR_Z * camHeightRatioR;
        double scatteredM = tauM * phaseM_Z * camHeightRatioM;

        // 消光（太阳光到散射点 + 散射点到观察者）
        double extinction = std::exp(-tauR[c] * (sunPathScale + 1.0) * camHeightRatioR) *
                            std::exp(-tauM * (sunPathScale + 1.0) * camHeightRatioM);

        // Ozone 吸收（主要在平流层）
        double ozoneOD = betaO[c] * p.ozoneDensityWidth * 2.0 *
                         ozoneDensity(cameraAltitudeMeters, p.ozoneDensityHeight, p.ozoneDensityWidth);

        double total = (scatteredR + scatteredM) * extinction *
                       std::exp(-ozoneOD) * p.sunIntensity * daylight;

        zenith_[c] = expose(total, 330.0);
    }
    zenith_[3] = 1.0f;

    // ========================================================
    // 地平线颜色 — 观察方向水平
    // ========================================================
    //
    // 地平线方向：viewDir = (cos(az), sin(az), 0) in local ENU
    // 散射角度依赖于方位角 — 平均来讲使用典型值
    // 朝向太阳：cos(θ) ≈ cos(elevation)，背向太阳：cos(θ) ≈ -cos(elevation)
    // 这里使用正面散射（朝向太阳方向）作为地平线亮度的主要贡献
    //
    double cosThetaHorizon =
        std::sqrt(std::max(1.0 - sunDotUp * sunDotUp, 0.0)) * 0.7;

    double phaseR_H = rayleighPhase(cosThetaHorizon);
    double phaseM_H = miePhase(cosThetaHorizon);

    // 水平路径光学深度远大于垂直路径
    // τ_horiz ≈ τ_vertical * 40（典型大气质量在水平方向约 40 倍垂直）
    double horizMassFactor = 38.0; // air mass at horizon ≈ 38

    for (int c = 0; c < 3; ++c) {
        double scatteredR = tauR[c] * phaseR_H * horizMassFactor *
                            camHeightRatioR;
        double scatteredM = tauM * phaseM_H * horizMassFactor *
                            camHeightRatioM;

        // 消光（水平路径消光很大）
        double extinction =
            std::exp(-tauR[c] * (sunPathScale + horizMassFactor * 0.35) * camHeightRatioR) *
            std::exp(-tauM * (sunPathScale + horizMassFactor * 0.35) * camHeightRatioM);

        double ozoneOD = betaO[c] * p.ozoneDensityWidth * 2.0 *
                         ozoneDensity(cameraAltitudeMeters, p.ozoneDensityHeight, p.ozoneDensityWidth);

        double total = (scatteredR + scatteredM) * extinction *
                       std::exp(-ozoneOD) * p.sunIntensity * daylight;

        // 地面反照率二次散射
        double secondaryScattering = p.groundAlbedo * tauR[c] *
                                     std::exp(-tauR[c] * horizMassFactor * 0.5) *
                                     phaseR_H * 0.1 * camHeightRatioR * daylight;
        total += secondaryScattering;

        horizon_[c] = expose(total, 30.0);
    }

    // 日出/日落暖色调增强
    // 当太阳靠近地平线时，蓝光被强烈散射，Mie 前向散射产生红/橙色
    double absElev = std::abs(sunElevation_);
    if (sunElevation_ > -0.08 && absElev < 0.3) { // < ~17° above horizon
        double twilight = 1.0 - absElev / 0.3;
        twilight = twilight * twilight *
            smoothstep(-0.08, 0.04, sunElevation_); // smooth falloff

        // 增强红色和绿色分量（Mie 散射暖色）
        horizon_[0] = std::clamp(horizon_[0] + static_cast<float>(twilight * 0.4), 0.0f, 1.0f);
        horizon_[1] = std::clamp(horizon_[1] + static_cast<float>(twilight * 0.15), 0.0f, 1.0f);
        // 蓝色在低角度时被衰减
        horizon_[2] = std::clamp(horizon_[2] - static_cast<float>(twilight * 0.1), 0.0f, 1.0f);
    }
    horizon_[3] = 1.0f;

    // ========================================================
    // 环境光 — 天空半球对地面的最低照明
    // ========================================================
    //
    // 环境光 ≈ 天顶颜色 + 地平线颜色的混合，代表从天空半球到达地面的漫射光
    // 在白天 ≈ (zenith + horizon)/2 * 0.5
    // 在夜晚 ≈ 非常暗
    for (int c = 0; c < 3; ++c) {
        double amb = (zenith_[c] * 0.6 + horizon_[c] * 0.4) * 0.35;
        // 保证最低亮度（星光）
        amb = std::max(amb, 0.005);
        ambient_[c] = static_cast<float>(amb);
    }

    // 夜晚：太阳在地平线以下时，环境光急剧下降
    if (sunElevation_ < -0.05) {
        double nightFactor = std::exp(sunElevation_ * 8.0); // 快速衰减
        nightFactor = std::clamp(nightFactor, 0.0, 1.0);
        for (int c = 0; c < 3; ++c) {
            ambient_[c] = std::max(ambient_[c] * static_cast<float>(nightFactor), 0.002f);
        }
    }
}

} // namespace earth_engine
