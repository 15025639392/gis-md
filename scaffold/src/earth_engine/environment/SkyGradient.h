#pragma once

#include "../core/math/Vec3.h"
#include <array>

namespace earth_engine {

/// 天空颜色渐变（visual-only 等级）。
///
/// 根据太阳方向计算：
///   - 天顶颜色（天空顶部）
///   - 地平线颜色（天空底部，clear color 使用）
///   - 环境光颜色
///
/// 使用简化的 Rayleigh 散射近似，不涉及真实大气物理。
class SkyGradient {
public:
    /// 根据 ECEF 太阳方向更新颜色
    /// @param sunDirECEF 太阳方向单位向量（地心→太阳）
    void update(const Vec3& sunDirECEF);

    /// 天顶颜色（RGBA，0..1）
    const std::array<float, 4>& zenithColor() const { return zenith_; }

    /// 地平线/clear 颜色（RGBA，0..1）
    const std::array<float, 4>& horizonColor() const { return horizon_; }

    /// 环境光颜色（RGB，0..1，用于最低 ambient 照明）
    const std::array<float, 3>& ambientColor() const { return ambient_; }

    /// 太阳仰角（radian，0=地平线，π/2=头顶）
    double sunElevation() const { return sunElevation_; }

private:
    std::array<float, 4> zenith_;
    std::array<float, 4> horizon_;
    std::array<float, 3> ambient_;
    double sunElevation_ = 0.0;
};

} // namespace earth_engine
