#pragma once

#include "../core/math/Vec3.h"
#include "AtmosphereParameters.h"
#include <array>

namespace earth_engine {

/// 物理大气散射颜色计算。
///
/// 使用 Rayleigh + Mie 散射解析近似，与 openglobus/src/shaders/atmos/
/// 使用相同的参数模型，但不依赖 LUT 预计算。
///
/// 根据太阳方向 + 相机高度计算：
///   - 天顶颜色（天空顶部）
///   - 地平线颜色（天空底部，clear color 使用）
///   - 环境光颜色
class SkyGradient {
public:
    SkyGradient();
    explicit SkyGradient(const AtmosphereParameters& params);

    /// 设置大气参数
    void setParameters(const AtmosphereParameters& params);

    /// 获取当前大气参数
    const AtmosphereParameters& parameters() const { return params_; }

    /// 根据 ECEF 太阳方向、当前位置椭球法线 + 相机海拔高度更新颜色
    /// @param sunDirECEF 太阳方向单位向量（地心→太阳）
    /// @param localUpECEF 当前观察位置的椭球面外法线（单位向量）
    /// @param cameraAltitudeMeters 相机距椭球表面高度（米），默认 0（地面）
    void update(const Vec3& sunDirECEF,
                const Vec3& localUpECEF,
                double cameraAltitudeMeters = 0.0);

    /// 天顶颜色（RGBA，0..1）
    const std::array<float, 4>& zenithColor() const { return zenith_; }

    /// 地平线/clear 颜色（RGBA，0..1）
    const std::array<float, 4>& horizonColor() const { return horizon_; }

    /// 环境光颜色（RGB，0..1，用于最低 ambient 照明）
    const std::array<float, 3>& ambientColor() const { return ambient_; }

    /// 太阳仰角（radian，0=地平线，π/2=头顶）
    double sunElevation() const { return sunElevation_; }

private:
    AtmosphereParameters params_;
    std::array<float, 4> zenith_;
    std::array<float, 4> horizon_;
    std::array<float, 3> ambient_;
    double sunElevation_ = 0.0;
};

} // namespace earth_engine
