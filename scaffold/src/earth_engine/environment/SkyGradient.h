#pragma once

#include "../core/math/Vec3.h"
#include "AtmosphereParameters.h"
#include <array>

namespace earth_engine {

/// 天空 clear/环境光颜色(与 GPU 天空同一色模型)。
///
/// L-P1:此前这里是独立的物理 Rayleigh+Mie+Ozone 解析散射实现,与 GLSL
/// computeSkyColor(经验色板)数值漂移 → 清屏色/环境光与用户看到的天空不配套。
/// 现改为直接采样 computeSkyColorCpu(SkyColorModel.h 单一事实源):
///   - 天顶色/地平线色 = computeSkyColor 对应方向采样(与 GPU 天空/雾同源)
///   - 环境光 = 天顶/地平线加权(保留原 0.6/0.4 半球近似)
///   - 夜晚门控保留(computeSkyColor 无夜晚语义,SkyBox starfield 由
///     SceneRenderPipeline 按同式 nightFactor 叠加)
///
/// 根据太阳方向 + 相机高度计算：
///   - 天顶颜色（天空顶部）
///   - 地平线颜色（天空底部，clear color 使用）
///   - 环境光颜色（地表最低补光）
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

    /// 日落地表着色强度(运行时可配,来源 EarthSceneConfig)。warmth=受光面暖度、
    /// shadowScale=阴影暖补光缩放;色基准在 SceneFrameStateBuilder 的编译期档位。
    /// 承载于此仅因 SkyGradient 已穿到 frameState builder,非大气散射本身参数。
    void setSunsetTerrainTint(float warmth, float shadowScale) {
        sunsetWarmth_ = warmth;
        sunsetShadowScale_ = shadowScale;
    }
    float sunsetWarmth() const { return sunsetWarmth_; }
    float sunsetShadowScale() const { return sunsetShadowScale_; }

private:
    AtmosphereParameters params_;
    std::array<float, 4> zenith_;
    std::array<float, 4> horizon_;
    std::array<float, 3> ambient_;
    double sunElevation_ = 0.0;
    float sunsetWarmth_ = 0.75f;       // = kSunsetTintNatural.warmth 默认
    float sunsetShadowScale_ = 1.0f;   // 阴影暖补光缩放(1=自然档基准)
};

} // namespace earth_engine
