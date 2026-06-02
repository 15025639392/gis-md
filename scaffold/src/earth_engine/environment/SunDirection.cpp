#include "SunDirection.h"
#include "../core/geodesy/Ellipsoid.h"
#include "../core/geodesy/Cartographic.h"

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <cmath>
#include <algorithm>

namespace earth_engine {

// ============================================================
// Meeus 简化太阳位置（精度 ~0.5°）
// ============================================================

namespace {

/// Julian century since J2000.0
double julianCentury(double jd) {
    return (jd - 2451545.0) / 36525.0;
}

/// 经度归一化到 [0, 360)
double normalize360(double deg) {
    deg = std::fmod(deg, 360.0);
    if (deg < 0.0) deg += 360.0;
    return deg;
}

/// 太阳黄经（degree）
double sunEclipticLongitude(double T) {
    // Mean anomaly（度）
    double M = normalize360(357.5291 + 35999.0503 * T - 0.0001559 * T * T);
    double Mrad = glm::radians(M);

    // Equation of center
    double C = (1.9148 - 0.0048 * T - 0.000014 * T * T) * std::sin(Mrad)
             + (0.019993 - 0.000101 * T) * std::sin(2.0 * Mrad)
             + 0.000290 * std::sin(3.0 * Mrad);

    // Mean longitude
    double L0 = normalize360(280.4665 + 36000.7698 * T);

    return normalize360(L0 + C);
}

/// 黄赤交角（度）
double obliquity(double T) {
    return 23.4393 - 0.0130 * T;
}

} // anonymous namespace

// ============================================================
// SunDirection
// ============================================================

Vec3 SunDirection::compute(double julianDate) {
    double T = julianCentury(julianDate);
    double lon = glm::radians(sunEclipticLongitude(T));
    double obl = glm::radians(obliquity(T));

    // 黄道坐标 → 赤道坐标（J2000 春分点）
    double cosLon = std::cos(lon);
    double sinLon = std::sin(lon);
    double cosObl = std::cos(obl);
    double sinObl = std::sin(obl);

    // 赤道直角坐标（地心天球，X 指向春分点）
    double eqX = cosLon;
    double eqY = sinLon * cosObl;
    double eqZ = sinLon * sinObl;

    // 格林威治恒星时（近似）→ ECEF X 轴旋转
    // GMST ≈ 280.4606 + 360.9856474 * days_since_J2000
    double daysSinceJ2000 = julianDate - 2451545.0;
    double gmst = glm::radians(normalize360(280.4606 + 360.9856474 * daysSinceJ2000));

    double cosG = std::cos(gmst);
    double sinG = std::sin(gmst);

    // 旋转到 ECEF
    double ecefX = eqX * cosG + eqY * sinG;
    double ecefY = -eqX * sinG + eqY * cosG;
    double ecefZ = eqZ;

    // 归一化
    double len = std::sqrt(ecefX * ecefX + ecefY * ecefY + ecefZ * ecefZ);
    if (len < 1e-12) return Vec3(1, 0, 0);

    return Vec3(ecefX / len, ecefY / len, ecefZ / len);
}

double SunDirection::elevation(double julianDate) {
    Vec3 dir = compute(julianDate);
    // 太阳方向与赤道面的夹角 ≈ asin(z)
    return std::asin(dir.z());
}

double SunDirection::cosIncidence(double julianDate,
                                   double lngRad, double latRad) {
    Vec3 sunDir = compute(julianDate);
    Vec3 normal = Ellipsoid::WGS84().geodeticSurfaceNormal(
        Cartographic::fromRadians(lngRad, latRad, 0.0));
    double dot = sunDir.dot(normal);
    return std::max(0.0, dot);
}

} // namespace earth_engine
