#include "SunDirection.h"
#include "../core/geodesy/Ellipsoid.h"
#include "../core/geodesy/Cartographic.h"

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <cmath>
#include <algorithm>

namespace earth_engine {

// ============================================================
// OpenGlobus getSunPosition (stjarnhimlen.se tutorial)
// Matches src/astro/earth.ts exactly.
// ============================================================

namespace {

// OpenGlobus astro constants
constexpr double kJ2000 = 2451545.0;
constexpr double kObliquityDeg = 23.4392911;        // J2000_OBLIQUITY
constexpr double kAuToMeters = 1.4959787e11;         // AU_TO_METERS

double rev(double angleDeg) {
    double x = std::fmod(angleDeg, 360.0);
    if (x < 0.0) x += 360.0;
    return x;
}

} // anonymous namespace

Vec3 SunDirection::compute(double julianDate) {
    const double d = julianDate - kJ2000;                     // days since J2000.0
    const double w = rev(282.9404 + 4.70935e-5 * d);          // longitude of perihelion (°)
    const double e = 0.016709 - 1.151e-9 * d;                // eccentricity
    const double M_deg = rev(356.047 + 0.9856002585 * d);     // mean anomaly (°)
    const double oblecl_deg = kObliquityDeg - 3.563e-7 * d;  // obliquity of ecliptic (°)

    const double M = glm::radians(M_deg);
    const double oblecl = glm::radians(oblecl_deg);

    // Eccentric anomaly (approximation): E = M + e*sin(M)*(1 + e*cos(M))
    const double E_deg = M_deg +
        (180.0 / glm::pi<double>()) * e * std::sin(M) * (1.0 + e * std::cos(M));
    const double E = glm::radians(E_deg);

    // Heliocentric rectangular coordinates (perihelion frame)
    const double x_peri = std::cos(E) - e;
    const double y_peri = std::sin(E) * std::sqrt(1.0 - e * e);

    const double r = std::sqrt(x_peri * x_peri + y_peri * y_peri); // distance (AU)
    const double v = std::atan2(y_peri, x_peri);                     // true anomaly (rad)

    const double lon = glm::radians(rev(glm::degrees(v) + w));       // ecliptic longitude (rad)

    // Ecliptic → equatorial coordinates
    const double x_ecl = r * std::cos(lon);
    const double y_ecl = r * std::sin(lon);

    const double xequat = x_ecl;
    const double yequat = y_ecl * std::cos(oblecl);
    const double zequat = y_ecl * std::sin(oblecl);

    // Sidereal rotation: Earth rotates to align equatorial frame with ECEF
    const double theta = glm::two_pi<double>() *
        ((d * 24.0) / 23.9344694 - 259.853 / 360.0);

    // OpenGlobus: Quat.zRotation(-theta).mulVec3(Vec3(-xequat*AU, -yequat*AU, zequat*AU))
    const double cosT = std::cos(-theta);
    const double sinT = std::sin(-theta);

    const double sx = -xequat * kAuToMeters;
    const double sy = -yequat * kAuToMeters;
    const double sz =  zequat * kAuToMeters;

    const double ecefX = sx * cosT - sy * sinT;   // Z-rotation: x' = x*cos - y*sin
    const double ecefY = sx * sinT + sy * cosT;   //              y' = x*sin + y*cos
    const double ecefZ = sz;

    const double len = std::sqrt(ecefX * ecefX + ecefY * ecefY + ecefZ * ecefZ);
    if (len < 1e-12) return Vec3(1, 0, 0);

    return Vec3(ecefX / len, ecefY / len, ecefZ / len);
}

double SunDirection::elevation(double julianDate) {
    Vec3 dir = compute(julianDate);
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
