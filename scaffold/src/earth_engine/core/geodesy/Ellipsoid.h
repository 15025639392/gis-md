#pragma once

#include "Cartographic.h"
#include "../math/RayEllipsoidIntersectionInterval.h"
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
    Ellipsoid(double radiusX, double radiusY, double radiusZ);

    double semiMajorAxis() const { return radii_.x(); }
    double semiMinorAxis() const { return radii_.z(); }
    double flattening() const { return f_; }
    const Vec3& radii() const { return radii_; }
    double maximumRadius() const;
    double minimumRadius() const;

    /// 椭球上方的大地纬度高 → ECEF（单位：米）
    Vec3 cartographicToCartesian(const Cartographic& cart) const;

    /// ECEF → 大地坐标（迭代法）。中心点无法表达为唯一大地坐标；
    /// 需要区分失败时使用 tryCartesianToCartographic。
    Cartographic cartesianToCartographic(const Vec3& ecef) const;

    /// ECEF → 大地坐标。语义对齐 cesium-native：椭球中心返回 std::nullopt。
    std::optional<Cartographic> tryCartesianToCartographic(const Vec3& ecef) const;

    /// 大地坐标处椭球面外法线（单位向量）
    Vec3 geodeticSurfaceNormal(const Cartographic& cart) const;

    /// ECEF 椭球面外法线（单位向量）
    Vec3 geodeticSurfaceNormal(const Vec3& ecef) const;

    /// 将 ECEF 空间点投影到椭球面。输入输出单位：meter。
    /// 中心点无法投影；需要区分失败时使用 tryScaleToGeodeticSurface。
    Vec3 projectToSurface(const Vec3& point) const;

    /// 将空间点缩放到椭球表面。
    Vec3 scaleToGeodeticSurface(const Vec3& point) const;

    /// 将空间点缩放到椭球表面。语义对齐 cesium-native：椭球中心返回 std::nullopt。
    std::optional<Vec3> tryScaleToGeodeticSurface(const Vec3& point) const;

    /// 将空间点沿地心方向缩放到椭球表面。
    /// 中心点无法投影；需要区分失败时使用 tryScaleToGeocentricSurface。
    Vec3 scaleToGeocentricSurface(const Vec3& point) const;

    /// 将空间点沿地心方向缩放到椭球表面。语义对齐 cesium-native：
    /// 椭球中心返回 std::nullopt。
    std::optional<Vec3> tryScaleToGeocentricSurface(const Vec3& point) const;

    /// 射线与椭球相交。origin 单位 meter，direction 可非单位向量。
    /// miss 返回 std::nullopt，不使用零向量表达 miss。
    std::optional<Vec3> rayIntersection(const Vec3& origin,
                                        const Vec3& direction) const;

    /// 射线与椭球相交的参数区间。语义对齐 cesium-native rayEllipsoid：
    /// entryDistance 是入射参数，exitDistance 是出射参数。
    std::optional<RayEllipsoidIntersectionInterval>
    rayIntersectionInterval(const Vec3& origin,
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
    /// Unit sphere preset matching cesium-native Ellipsoid::UNIT_SPHERE.
    static const Ellipsoid& UNIT_SPHERE();

private:
    Vec3 radii_;
    Vec3 radiiSquared_;
    Vec3 oneOverRadii_;
    Vec3 oneOverRadiiSquared_;
    double f_;   // flattening for rotational ellipsoid geodesics
    double e2_;  // 第一偏心率平方, based on x/z radii
};

} // namespace earth_engine
