#pragma once

#include "Cartographic.h"
#include "../math/Vec3.h"

namespace earth_engine {

/// WGS84 参考椭球体。
/// 提供 cartographic ↔ ECEF 坐标转换。
class Ellipsoid {
public:
    Ellipsoid(double semiMajorAxis, double semiMinorAxis);

    double semiMajorAxis() const { return a_; }
    double semiMinorAxis() const { return b_; }
    double flattening() const { return f_; }

    /// 椭球上方的大地纬度高 → ECEF（单位：米）
    Vec3 cartographicToCartesian(const Cartographic& cart) const;

    /// ECEF → 大地坐标（迭代法）
    Cartographic cartesianToCartographic(const Vec3& ecef) const;

    /// 大地坐标处椭球面外法线（单位向量）
    Vec3 geodeticSurfaceNormal(const Cartographic& cart) const;

    /// 将空间点缩放到椭球表面
    Vec3 scaleToGeodeticSurface(const Vec3& point) const;

    /// WGS84 预置实例
    static const Ellipsoid& WGS84();

private:
    double a_;   // semi-major axis (meters)
    double b_;   // semi-minor axis (meters)
    double f_;   // flattening
    double e2_;  // 第一偏心率平方
};

} // namespace earth_engine
