#pragma once

#include "Cartographic.h"
#include "../math/Vec3.h"
#include <optional>

namespace earth_engine {

struct GeodesicInverseResult {
    double distanceMeters = 0.0;
    double initialAzimuthRadians = 0.0;
    double finalAzimuthRadians = 0.0;
    bool converged = false;
};

struct GeodesicDirectResult {
    Cartographic destination;
    double finalAzimuthRadians = 0.0;
    bool converged = false;
};

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

    /// ECEF 椭球面外法线（单位向量）
    Vec3 geodeticSurfaceNormal(const Vec3& ecef) const;

    /// 将 ECEF 空间点投影到椭球面。输入输出单位：meter。
    /// 语义对齐 OpenGlobus Ellipsoid.projToSurface。
    Vec3 projectToSurface(const Vec3& point) const;

    /// 将空间点缩放到椭球表面
    Vec3 scaleToGeodeticSurface(const Vec3& point) const;

    /// 射线与椭球相交。origin 单位 meter，direction 可非单位向量。
    /// miss 返回 std::nullopt，不使用零向量表达 miss。
    std::optional<Vec3> rayIntersection(const Vec3& origin,
                                        const Vec3& direction) const;

    /// Vincenty inverse：返回两点椭球测地线距离和起止方位角。
    GeodesicInverseResult inverse(const Cartographic& start,
                                  const Cartographic& end) const;

    /// Vincenty direct：从起点、初始方位角和距离求终点。
    GeodesicDirectResult direct(const Cartographic& start,
                                double initialAzimuthRadians,
                                double distanceMeters) const;

    /// WGS84 预置实例
    static const Ellipsoid& WGS84();

private:
    double a_;   // semi-major axis (meters)
    double b_;   // semi-minor axis (meters)
    double f_;   // flattening
    double e2_;  // 第一偏心率平方
};

} // namespace earth_engine
