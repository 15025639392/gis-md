#pragma once

/// 大气散射参数 — 与 openglobus/src/shaders/atmos/atmos.ts DEFAULT_PARAMS 对齐。
///
/// 基于 Bruneton 2008 "Precomputed Atmospheric Scattering" 模型的参数化。
/// 天顶/地平线/环境光颜色通过解析近似计算（不依赖 LUT 预计算），
/// 但参数结构保留以支持将来升级到完整 LUT 路径。
namespace earth_engine {

struct AtmosphereParameters {
    /// 大气层顶高度（米），默认 100km
    double atmosHeight = 100000.0;

    /// Rayleigh 散射高度尺度（米），默认 7994m（地球大气的 scale height ≈ 8km）
    double rayleighScaleHeight = 7994.0;

    /// Mie 散射高度尺度（米），默认 1200m（雾/气溶胶集中在低空）
    double mieScaleHeight = 1200.0;

    /// Rayleigh 散射系数 at sea level（m⁻¹），对应 openglobus RAYLEIGH_SCALE
    /// 在海平面上可归一化，实际衰减由 scale height 决定
    double rayleighSeaLevelScattering = 0.056;

    /// Mie 散射系数 at sea level（m⁻¹），对应 openglobus MIE_SCALE
    double mieSeaLevelScattering = 0.0045;

    /// 地面反照率（用于二次散射近似），对应 openglobus GROUND_ALBEDO
    double groundAlbedo = 0.022;

    /// 椭球赤道半径（米），WGS84 = 6378137.0
    double equatorialRadius = 6378137.0;

    /// 椭球极半径（米），WGS84 = 6356752.314
    /// 大气从地表计算，使用底部半径
    double bottomRadius = 6356752.314;

    /// Rayleigh 散射系数（RGB，m⁻¹），海平面值
    /// 对齐 openglobus rayleighScatteringCoefficient_0/1/2
    struct RayleighCoefficients {
        double r = 3.9;   // 红 (680nm)
        double g = 10.8;  // 绿 (550nm)
        double b = 20.0;  // 蓝 (440nm)
    } rayleigh;

    /// Mie 散射系数（海平面，m⁻¹）
    struct MieCoefficients {
        double scattering = 1.6;
        double extinction = 1.85;
    } mie;

    /// Ozone 吸收系数（海平面，m⁻¹）
    struct OzoneCoefficients {
        double r = 1.25;
        double g = 2.1;
        double b = 0.09;
    } ozone;

    /// Ozone 密度分布参数（米）
    double ozoneDensityHeight = 25000.0;  // 臭氧层中心高度
    double ozoneDensityWidth = 15000.0;   // 臭氧层半宽

    /// 太阳角半径（rad）。真实值 ≈ 0.00465 rad（0.267°，日面视直径 0.533°）——
    /// 从太空看即为远处的小亮盘，符合真实日地距离。光照方向用天文方向。
    double sunAngularRadius = 0.00465;

    /// 太阳强度倍数
    double sunIntensity = 0.78;

    /// 是否禁用太阳光盘渲染
    bool disableSunDisk = false;

    // === 预计算常量 ===

    /// 大气层顶半径 = bottomRadius + atmosHeight
    double topRadius() const { return bottomRadius + atmosHeight; }

    /// 预计算常用归一化因子
    void validate() {
        // 确保正值
        if (atmosHeight <= 0.0) atmosHeight = 100000.0;
        if (rayleighScaleHeight <= 0.0) rayleighScaleHeight = 7994.0;
        if (mieScaleHeight <= 0.0) mieScaleHeight = 1200.0;
    }
};

/// 地球大气默认参数（与 OpenGlobus DEFAULT_PARAMS 对齐）
inline AtmosphereParameters earthAtmosphereDefaults() {
    return AtmosphereParameters{};
}

/// 火星大气参数（与 OpenGlobus MARS_PARAMS 对齐）
inline AtmosphereParameters marsAtmosphereDefaults() {
    AtmosphereParameters p;
    p.atmosHeight = 60000.0;
    p.rayleighScaleHeight = 11000.0;
    p.mieScaleHeight = 8000.0;
    p.rayleighSeaLevelScattering = 0.18;
    p.mieSeaLevelScattering = 0.08;
    p.groundAlbedo = 0.15;
    p.bottomRadius = 3389508.0;
    p.equatorialRadius = 3396200.0;
    p.rayleigh = {1.0, 1.2, 1.4};
    p.mie = {10.0, 20.0};
    p.ozone = {0.1, 0.15, 0.8};
    p.ozoneDensityHeight = 10000.0;
    p.ozoneDensityWidth = 30000.0;
    p.sunIntensity = 1.0;
    return p;
}

} // namespace earth_engine
