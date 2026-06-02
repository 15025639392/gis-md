#pragma once

#include "../core/math/Vec3.h"

namespace earth_engine {

/// 简化天文太阳方向计算（approximate 等级，精度 ~0.5°）。
///
/// 输入：Julian Date（TT）
/// 输出：ECEF 坐标系中的太阳方向单位向量（从地球中心指向太阳）。
///
/// 实现 Meeus 简化公式：
///   - 太阳黄经（mean longitude + equation of center）
///   - 黄赤交角
///   - 转换为 ECEF（Earth-Centered Earth-Fixed）直角坐标
///
/// ECEF 约定（与 Ellipsoid 一致）：
///   +X: 赤道与经度 0 交点
///   +Y: 赤道与东经 90° 交点
///   +Z: 北极
class SunDirection {
public:
    /// 计算 ECEF 太阳方向（单位向量）
    static Vec3 compute(double julianDate);

    /// 计算太阳仰角（radian，相对于赤道面）
    static double elevation(double julianDate);

    /// 太阳方向与指定经纬度地表法线的夹角余弦（用于 diffuse）
    /// @return cos(incidence)，0..1 范围（clamped）
    static double cosIncidence(double julianDate,
                               double lngRad, double latRad);
};

} // namespace earth_engine
