#include "Ellipsoid.h"
#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <cmath>
#include <algorithm>

namespace earth_engine {

// ============================================================
// Cartographic 实现
// ============================================================

namespace {
    constexpr double kRadToDeg = 180.0 / glm::pi<double>();
    constexpr double kDegToRad = glm::pi<double>() / 180.0;
}

Cartographic Cartographic::fromDegrees(double lngDeg, double latDeg, double heightM) {
    Cartographic c;
    c.lng_ = lngDeg * kDegToRad;
    c.lat_ = latDeg * kDegToRad;
    c.height_ = heightM;
    return c;
}

double Cartographic::longitudeDegrees() const { return lng_ * kRadToDeg; }
double Cartographic::latitudeDegrees() const { return lat_ * kRadToDeg; }

bool Cartographic::operator==(const Cartographic& rhs) const {
    return lng_ == rhs.lng_ && lat_ == rhs.lat_ && height_ == rhs.height_;
}
bool Cartographic::operator!=(const Cartographic& rhs) const { return !(*this == rhs); }

std::ostream& operator<<(std::ostream& os, const Cartographic& c) {
    return os << "Cartographic(lng:" << c.longitudeDegrees() << "°, lat:"
              << c.latitudeDegrees() << "°, h:" << c.height() << "m)";
}

// ============================================================
// Ellipsoid 实现
// ============================================================

Ellipsoid::Ellipsoid(double semiMajorAxis, double semiMinorAxis)
    : a_(semiMajorAxis), b_(semiMinorAxis) {
    f_ = (a_ - b_) / a_;
    e2_ = 2.0 * f_ - f_ * f_;
}

Vec3 Ellipsoid::cartographicToCartesian(const Cartographic& cart) const {
    double lng = cart.longitude();
    double lat = cart.latitude();
    double h = cart.height();

    double sinLat = std::sin(lat);
    double cosLat = std::cos(lat);
    double sinLng = std::sin(lng);
    double cosLng = std::cos(lng);

    // Prime vertical radius of curvature
    double N = a_ / std::sqrt(1.0 - e2_ * sinLat * sinLat);

    double x = (N + h) * cosLat * cosLng;
    double y = (N + h) * cosLat * sinLng;
    double z = (N * (1.0 - e2_) + h) * sinLat;

    return Vec3(x, y, z);
}

Cartographic Ellipsoid::cartesianToCartographic(const Vec3& ecef) const {
    // 迭代法（Bowring 1985 / standard approach）
    double x = ecef.x();
    double y = ecef.y();
    double z = ecef.z();

    double lng = std::atan2(y, x);

    double p = std::sqrt(x * x + y * y);
    double lat = std::atan2(z, p * (1.0 - e2_));  // 初值

    // 迭代（通常 3-5 次足够）
    for (int i = 0; i < 5; ++i) {
        double sinLat = std::sin(lat);
        double N = a_ / std::sqrt(1.0 - e2_ * sinLat * sinLat);
        double h = p / std::cos(lat) - N;
        lat = std::atan2(z, p * (1.0 - e2_ * N / (N + h)));
    }

    double sinLat = std::sin(lat);
    double N = a_ / std::sqrt(1.0 - e2_ * sinLat * sinLat);
    double h = p / std::cos(lat) - N;

    // 处理极区
    if (p < 1e-6) {
        lat = (z > 0) ? glm::half_pi<double>() : -glm::half_pi<double>();
        h = std::abs(z) - b_;
    }

    return Cartographic::fromRadians(lng, lat, h);
}

Vec3 Ellipsoid::geodeticSurfaceNormal(const Cartographic& cart) const {
    double lat = cart.latitude();
    double lng = cart.longitude();
    double cosLat = std::cos(lat);
    return Vec3(cosLat * std::cos(lng), cosLat * std::sin(lng), std::sin(lat));
}

Vec3 Ellipsoid::scaleToGeodeticSurface(const Vec3& point) const {
    // 在 WGS84 椭球面上缩放点（使用单位球近似 + 偏心率修正）
    double x = point.x();
    double y = point.y();
    double z = point.z();

    double r2 = x * x + y * y + z * z;
    double r = std::sqrt(r2);
    if (r < 1e-12) return Vec3::zero();

    double beta = b_ / a_;
    double scale = a_ * beta / std::sqrt(beta * beta * (x * x + y * y) / r2 + z * z / r2);
    return point * (scale / r);
}

const Ellipsoid& Ellipsoid::WGS84() {
    static const Ellipsoid wgs84(6378137.0, 6356752.314245);
    return wgs84;
}

} // namespace earth_engine
