#include "SkyGradient.h"
#include <glm/gtc/constants.hpp>
#include <cmath>
#include <algorithm>

namespace earth_engine {

// ============================================================
// 简化的 Rayleigh 散射颜色查找
// ============================================================

namespace {

/// 太阳仰角 → 天顶 + 地平线颜色（visual-only）
/// 仰角：正值 = 白天，负值 = 夜晚
void skyColorsFromElevation(double elevationRad,
                             std::array<float, 4>& zenith,
                             std::array<float, 4>& horizon,
                             std::array<float, 3>& ambient) {
    // 映射仰角到 [0, 1]（-90° → 0, +90° → 1）
    double t = std::clamp((elevationRad + glm::half_pi<double>()) / glm::pi<double>(),
                          0.0, 1.0);

    // 天顶：头顶天空色
    // 白天：深蓝 (0.2, 0.4, 0.85)
    // 夜晚：深黑蓝 (0.01, 0.01, 0.06)
    double zR = 0.01 + t * 0.19;
    double zG = 0.01 + t * 0.39;
    double zB = 0.06 + t * 0.79;

    // 地平线：接近地面颜色
    // 白天：浅蓝白 (0.55, 0.7, 0.95)
    // 夜晚：深紫蓝 (0.02, 0.02, 0.08)
    double hR = 0.02 + t * 0.53;
    double hG = 0.02 + t * 0.68;
    double hB = 0.08 + t * 0.87;

    // 日出/日落暖色调增强（t ≈ 0.5 时最强）
    double twilight = 1.0 - std::abs(t - 0.5) * 4.0;  // 0..1，峰值在 t=0.5
    twilight = std::max(0.0, twilight);
    hR += twilight * 0.3;
    hG += twilight * 0.15;

    zenith = {static_cast<float>(zR), static_cast<float>(zG),
              static_cast<float>(zB), 1.0f};
    horizon = {static_cast<float>(hR), static_cast<float>(hG),
               static_cast<float>(hB), 1.0f};

    // 环境光：最低亮度保证黑夜不完全黑
    float amb = 0.03f + static_cast<float>(t) * 0.15f;
    ambient = {amb, amb, amb * 0.8f};
}

} // anonymous namespace

// ============================================================
// SkyGradient
// ============================================================

void SkyGradient::update(const Vec3& sunDirECEF) {
    // 太阳仰角 = asin(z)，其中 z 是 ECEF 中的北向分量
    sunElevation_ = std::asin(std::clamp(sunDirECEF.z(), -1.0, 1.0));

    skyColorsFromElevation(sunElevation_, zenith_, horizon_, ambient_);
}

} // namespace earth_engine
