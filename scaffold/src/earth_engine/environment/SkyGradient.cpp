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

/// 斜路径光学深度（平面平行大气近似）
/// Chapman 函数近似：τ(χ) ≈ τ_vertical / cos(χ)，χ < 80°
/// 更精确：τ(χ) ≈ τ_vertical * (1 / cos(χ)) * Chapman 修正
double opticalDepthSlant(double tauVertical, double cosZenithAngle) {
    double mu = std::max(std::abs(cosZenithAngle), 0.02); // 避免除零
    return tauVertical / mu;
}

/// 将线性 RGB 从 [0,1] 映射到 sRGB gamma
float linearToSRGB(float c) {
    if (c <= 0.0031308f)
        return 12.92f * c;
    return 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
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

void SkyGradient::update(const Vec3& sunDirECEF, double cameraAltitudeMeters) {
    // 太阳仰角 = asin(z)，ECEF +Z 轴指向北极
    // 在 ECEF 中，太阳方向的 Z 分量对应"北向"，但我们需要的是
    // 太阳相对于局部地平面的仰角
    //
    // 实际上对于天顶/地平线颜色，使用 sunDir 在世界空间的方向
    // 仰角近似：sunDir.z 是太阳在 ECEF 北极方向的分量
    // 对于地球尺度的散射，使用 geocentric 仰角是合理的近似
    sunElevation_ = std::asin(std::clamp(sunDirECEF.z(), -1.0, 1.0));

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
    double camHeightRatioR = std::exp(-cameraAltitudeMeters / p.rayleighScaleHeight);
    double camHeightRatioM = std::exp(-cameraAltitudeMeters / p.mieScaleHeight);

    // ========================================================
    // 天顶颜色 — 观察方向垂直向上
    // ========================================================
    //
    // viewDir = up (local zenith)
    // cos(scattering angle) = sunDir · viewDir = sin(sunElevation)
    // 因为 viewDir = (0,0,1) in local ENU, sunDir_ENU 的 z 分量 = sin(elevation)
    //
    double cosThetaZenith = std::sin(sunElevation_);

    double phaseR_Z = rayleighPhase(cosThetaZenith);
    double phaseM_Z = miePhase(cosThetaZenith);

    // 太阳光到散射点的光学深度（太阳在天顶角 π/2-elevation）
    double cosSunZenith = std::cos(glm::half_pi<double>() - sunElevation_);
    // 太阳光在大气中的路径：从大气层顶到海平面
    // 平面平行近似：τ_sun = τ_vertical / cos(sunZenithAngle)
    double sunPathScale = 1.0 / std::max(std::abs(cosSunZenith), 0.02);

    for (int c = 0; c < 3; ++c) {
        // 沿视线散射
        double scatteredR = betaR[c] * phaseR_Z * tauR[c] * camHeightRatioR;
        double scatteredM = betaM * phaseM_Z * tauM * camHeightRatioM;

        // 消光（太阳光到散射点 + 散射点到观察者）
        double extinction = std::exp(-tauR[c] * sunPathScale - tauR[c]) *
                            std::exp(-tauM * sunPathScale - tauM);

        // Ozone 吸收（主要在平流层）
        double ozoneOD = betaO[c] * p.ozoneDensityWidth * 2.0 *
                         ozoneDensity(cameraAltitudeMeters, p.ozoneDensityHeight, p.ozoneDensityWidth);

        double total = (scatteredR + scatteredM) * extinction *
                       std::exp(-ozoneOD) * p.sunIntensity;

        // 缩放到 [0,1] 范围（归一化因子）
        // 天顶的典型散射强度在海平面约为：
        // β_R_blue * p_R * H_R ≈ 20e-6 * 0.06 * 8000 ≈ 0.01
        // 需要乘以缩放因子使其在视觉上合理
        double scale = 6000.0; // empirical scaling to reach visible brightness
        zenith_[c] = std::clamp(static_cast<float>(total * scale), 0.0f, 1.0f);
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
    double cosThetaHorizon = std::cos(sunElevation_) * 0.7; // 加权朝向太阳

    double phaseR_H = rayleighPhase(cosThetaHorizon);
    double phaseM_H = miePhase(cosThetaHorizon);

    // 水平路径光学深度远大于垂直路径
    // τ_horiz ≈ τ_vertical * 40（典型大气质量在水平方向约 40 倍垂直）
    double horizMassFactor = 38.0; // air mass at horizon ≈ 38

    for (int c = 0; c < 3; ++c) {
        double scatteredR = betaR[c] * phaseR_H * tauR[c] * horizMassFactor *
                            camHeightRatioR;
        double scatteredM = betaM * phaseM_H * tauM * horizMassFactor *
                            camHeightRatioM;

        // 消光（水平路径消光很大）
        double extinction = std::exp(-tauR[c] * (sunPathScale + horizMassFactor)) *
                            std::exp(-tauM * (sunPathScale + horizMassFactor));

        double ozoneOD = betaO[c] * p.ozoneDensityWidth * 2.0 *
                         ozoneDensity(cameraAltitudeMeters, p.ozoneDensityHeight, p.ozoneDensityWidth);

        double total = (scatteredR + scatteredM) * extinction *
                       std::exp(-ozoneOD) * p.sunIntensity;

        // 地面反照率二次散射
        double secondaryScattering = p.groundAlbedo * tauR[c] *
                                     std::exp(-tauR[c] * horizMassFactor * 0.5) *
                                     phaseR_H * 0.1;
        total += secondaryScattering;

        // 缩放
        double scale = 6000.0;
        horizon_[c] = std::clamp(static_cast<float>(total * scale), 0.0f, 1.0f);
    }

    // 日出/日落暖色调增强
    // 当太阳靠近地平线时，蓝光被强烈散射，Mie 前向散射产生红/橙色
    double absElev = std::abs(sunElevation_);
    if (absElev < 0.3) { // < ~17° above horizon
        double twilight = 1.0 - absElev / 0.3;
        twilight = twilight * twilight; // smooth falloff

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
