#include "Ellipsoid.h"
#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <cmath>
#include <algorithm>

namespace earth_engine {

namespace {
    constexpr double kEpsilon1 = 1e-1;
    constexpr double kEpsilon12 = 1e-12;

    double normalizeTwoPi(double radians) {
        double x = std::fmod(radians, glm::two_pi<double>());
        if (x < 0.0) x += glm::two_pi<double>();
        return x;
    }
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
    auto cartographic = tryCartesianToCartographic(ecef);
    return cartographic.value_or(Cartographic::fromRadians(0.0, 0.0, 0.0));
}

std::optional<Cartographic> Ellipsoid::tryCartesianToCartographic(
    const Vec3& ecef) const {
    auto surface = tryScaleToGeodeticSurface(ecef);
    if (!surface) {
        return std::nullopt;
    }

    Vec3 normal = geodeticSurfaceNormal(*surface);
    Vec3 heightVec = ecef - *surface;

    double lng = std::atan2(normal.y(), normal.x());
    double lat = std::asin(std::clamp(normal.z(), -1.0, 1.0));
    double h = heightVec.length();
    if (heightVec.dot(ecef) < 0.0) {
        h = -h;
    }

    return Cartographic::fromRadians(lng, lat, h);
}

Vec3 Ellipsoid::geodeticSurfaceNormal(const Cartographic& cart) const {
    double lat = cart.latitude();
    double lng = cart.longitude();
    double cosLat = std::cos(lat);
    return Vec3(cosLat * std::cos(lng), cosLat * std::sin(lng), std::sin(lat));
}

Vec3 Ellipsoid::geodeticSurfaceNormal(const Vec3& ecef) const {
    const double nx = ecef.x() / (a_ * a_);
    const double ny = ecef.y() / (a_ * a_);
    const double nz = ecef.z() / (b_ * b_);
    const double len = std::sqrt(nx * nx + ny * ny + nz * nz);
    if (len < 1e-24) return Vec3::unitZ();
    return Vec3(nx / len, ny / len, nz / len);
}

Vec3 Ellipsoid::projectToSurface(const Vec3& point) const {
    return scaleToGeodeticSurface(point);
}

Vec3 Ellipsoid::scaleToGeodeticSurface(const Vec3& point) const {
    auto surface = tryScaleToGeodeticSurface(point);
    return surface.value_or(Vec3::zero());
}

std::optional<Vec3> Ellipsoid::tryScaleToGeodeticSurface(
    const Vec3& point) const {
    const double px = point.x();
    const double py = point.y();
    const double pz = point.z();

    const double invA = 1.0 / a_;
    const double invB = 1.0 / b_;
    const double invA2 = 1.0 / (a_ * a_);
    const double invB2 = 1.0 / (b_ * b_);

    const double x2 = px * px * invA * invA;
    const double y2 = py * py * invA * invA;
    const double z2 = pz * pz * invB * invB;

    const double squaredNorm = x2 + y2 + z2;
    const double ratio = std::sqrt(1.0 / squaredNorm);
    const Vec3 intersection = point * ratio;

    if (squaredNorm < kEpsilon1) {
        return std::isfinite(ratio) ? std::optional<Vec3>(intersection)
                                    : std::nullopt;
    }

    const Vec3 gradient(intersection.x() * invA2 * 2.0,
                        intersection.y() * invA2 * 2.0,
                        intersection.z() * invB2 * 2.0);
    double lambda =
        ((1.0 - ratio) * point.length()) / (0.5 * gradient.length());
    double correction = 0.0;

    double mx = 0.0;
    double my = 0.0;
    double mz = 0.0;
    double func = 0.0;
    do {
        lambda -= correction;

        mx = 1.0 / (1.0 + lambda * invA2);
        my = 1.0 / (1.0 + lambda * invA2);
        mz = 1.0 / (1.0 + lambda * invB2);

        const double mx2 = mx * mx;
        const double my2 = my * my;
        const double mz2 = mz * mz;
        const double mx3 = mx2 * mx;
        const double my3 = my2 * my;
        const double mz3 = mz2 * mz;

        func = x2 * mx2 + y2 * my2 + z2 * mz2 - 1.0;
        const double denominator =
            x2 * mx3 * invA2 +
            y2 * my3 * invA2 +
            z2 * mz3 * invB2;
        const double derivative = -2.0 * denominator;
        correction = func / derivative;
    } while (std::abs(func) > kEpsilon12);

    return Vec3(px * mx, py * my, pz * mz);
}

std::optional<Vec3> Ellipsoid::rayIntersection(const Vec3& origin,
                                               const Vec3& direction) const {
    auto interval = rayIntersectionInterval(origin, direction);
    if (!interval) {
        return std::nullopt;
    }
    return origin + direction * interval->entryDistance;
}

std::optional<RayEllipsoidIntersectionInterval>
Ellipsoid::rayIntersectionInterval(const Vec3& origin,
                                   const Vec3& direction) const {
    if (a_ == 0.0 || b_ == 0.0) {
        return std::nullopt;
    }

    const Vec3 q(origin.x() / a_, origin.y() / a_, origin.z() / b_);
    const Vec3 w(direction.x() / a_, direction.y() / a_, direction.z() / b_);
    const double q2 = q.dot(q);
    const double qw = q.dot(w);
    const double w2 = w.dot(w);
    if (w2 <= 0.0) return std::nullopt;

    double difference = 0.0;
    double product = 0.0;
    double discriminant = 0.0;
    double temp = 0.0;

    if (q2 > 1.0) {
        if (qw >= 0.0) {
            return std::nullopt;
        }
        const double qw2 = qw * qw;
        difference = q2 - 1.0;
        product = w2 * difference;
        if (qw2 < product) {
            return std::nullopt;
        }
        if (qw2 > product) {
            discriminant = qw2 - product;
            temp = -qw + std::sqrt(discriminant);
            const double root0 = temp / w2;
            const double root1 = difference / temp;
            return root0 < root1
                ? RayEllipsoidIntersectionInterval{root0, root1}
                : RayEllipsoidIntersectionInterval{root1, root0};
        }
        const double root = std::sqrt(difference / w2);
        return RayEllipsoidIntersectionInterval{root, root};
    }

    if (q2 < 1.0) {
        difference = q2 - 1.0;
        product = w2 * difference;
        discriminant = qw * qw - product;
        temp = -qw + std::sqrt(discriminant);
        return RayEllipsoidIntersectionInterval{0.0, temp / w2};
    }

    if (qw < 0.0) {
        return RayEllipsoidIntersectionInterval{0.0, -qw / w2};
    }
    return std::nullopt;
}

GeodesicInverseResult Ellipsoid::inverse(const Cartographic& start,
                                         const Cartographic& end) const {
    GeodesicInverseResult result;
    const double L = end.longitude() - start.longitude();
    const double tanU1 = (1.0 - f_) * std::tan(start.latitude());
    const double tanU2 = (1.0 - f_) * std::tan(end.latitude());
    const double cosU1 = 1.0 / std::sqrt(1.0 + tanU1 * tanU1);
    const double sinU1 = tanU1 * cosU1;
    const double cosU2 = 1.0 / std::sqrt(1.0 + tanU2 * tanU2);
    const double sinU2 = tanU2 * cosU2;

    double lambda = L;
    double previous = 0.0;
    double sinSigma = 0.0;
    double cosSigma = 1.0;
    double sigma = 0.0;
    double sinAlpha = 0.0;
    double cosSqAlpha = 1.0;
    double cos2SigmaM = 0.0;
    int iterations = 0;
    do {
        const double sinLambda = std::sin(lambda);
        const double cosLambda = std::cos(lambda);
        const double aTerm = cosU2 * sinLambda;
        const double bTerm = cosU1 * sinU2 - sinU1 * cosU2 * cosLambda;
        sinSigma = std::sqrt(aTerm * aTerm + bTerm * bTerm);
        if (sinSigma < 1e-24) {
            result.converged = true;
            return result;
        }
        cosSigma = sinU1 * sinU2 + cosU1 * cosU2 * cosLambda;
        sigma = std::atan2(sinSigma, cosSigma);
        sinAlpha = (cosU1 * cosU2 * sinLambda) / sinSigma;
        cosSqAlpha = 1.0 - sinAlpha * sinAlpha;
        cos2SigmaM = cosSqAlpha != 0.0
            ? cosSigma - (2.0 * sinU1 * sinU2) / cosSqAlpha
            : 0.0;
        const double C = (f_ / 16.0) * cosSqAlpha * (4.0 + f_ * (4.0 - 3.0 * cosSqAlpha));
        previous = lambda;
        lambda = L + (1.0 - C) * f_ * sinAlpha *
            (sigma + C * sinSigma *
                (cos2SigmaM + C * cosSigma *
                    (-1.0 + 2.0 * cos2SigmaM * cos2SigmaM)));
    } while (std::abs(lambda - previous) > kEpsilon12 && ++iterations < 1000);

    result.converged = iterations < 1000;
    const double uSq = cosSqAlpha * (a_ * a_ - b_ * b_) / (b_ * b_);
    const double A = 1.0 + (uSq / 16384.0) *
        (4096.0 + uSq * (-768.0 + uSq * (320.0 - 175.0 * uSq)));
    const double B = (uSq / 1024.0) *
        (256.0 + uSq * (-128.0 + uSq * (74.0 - 47.0 * uSq)));
    const double deltaSigma = B * sinSigma *
        (cos2SigmaM + (B / 4.0) *
            (cosSigma * (-1.0 + 2.0 * cos2SigmaM * cos2SigmaM) -
             (B / 6.0) * cos2SigmaM *
                (-3.0 + 4.0 * sinSigma * sinSigma) *
                (-3.0 + 4.0 * cos2SigmaM * cos2SigmaM)));

    result.distanceMeters = b_ * A * (sigma - deltaSigma);
    result.initialAzimuthRadians = normalizeTwoPi(std::atan2(
        cosU2 * std::sin(lambda),
        cosU1 * sinU2 - sinU1 * cosU2 * std::cos(lambda)));
    result.finalAzimuthRadians = normalizeTwoPi(std::atan2(
        cosU1 * std::sin(lambda),
        -sinU1 * cosU2 + cosU1 * sinU2 * std::cos(lambda)));
    return result;
}

GeodesicDirectResult Ellipsoid::direct(const Cartographic& start,
                                       double initialAzimuthRadians,
                                       double distanceMeters) const {
    GeodesicDirectResult result;
    const double sinAlpha1 = std::sin(initialAzimuthRadians);
    const double cosAlpha1 = std::cos(initialAzimuthRadians);
    const double tanU1 = (1.0 - f_) * std::tan(start.latitude());
    const double cosU1 = 1.0 / std::sqrt(1.0 + tanU1 * tanU1);
    const double sinU1 = tanU1 * cosU1;
    const double sigma1 = std::atan2(tanU1, cosAlpha1);
    const double sinAlpha = cosU1 * sinAlpha1;
    const double cosSqAlpha = 1.0 - sinAlpha * sinAlpha;
    const double uSq = cosSqAlpha * (a_ * a_ - b_ * b_) / (b_ * b_);
    const double A = 1.0 + (uSq / 16384.0) *
        (4096.0 + uSq * (-768.0 + uSq * (320.0 - 175.0 * uSq)));
    const double B = (uSq / 1024.0) *
        (256.0 + uSq * (-128.0 + uSq * (74.0 - 47.0 * uSq)));

    double sigma = distanceMeters / (b_ * A);
    double previous = 0.0;
    double cos2SigmaM = 0.0;
    double sinSigma = 0.0;
    double cosSigma = 0.0;
    int iterations = 0;
    do {
        cos2SigmaM = std::cos(2.0 * sigma1 + sigma);
        sinSigma = std::sin(sigma);
        cosSigma = std::cos(sigma);
        const double deltaSigma = B * sinSigma *
            (cos2SigmaM + (B / 4.0) *
                (cosSigma * (-1.0 + 2.0 * cos2SigmaM * cos2SigmaM) -
                 (B / 6.0) * cos2SigmaM *
                    (-3.0 + 4.0 * sinSigma * sinSigma) *
                    (-3.0 + 4.0 * cos2SigmaM * cos2SigmaM)));
        previous = sigma;
        sigma = distanceMeters / (b_ * A) + deltaSigma;
    } while (std::abs(sigma - previous) > kEpsilon12 && ++iterations < 1000);

    const double tmp = sinU1 * sinSigma - cosU1 * cosSigma * cosAlpha1;
    const double lat2 = std::atan2(
        sinU1 * cosSigma + cosU1 * sinSigma * cosAlpha1,
        (1.0 - f_) * std::sqrt(sinAlpha * sinAlpha + tmp * tmp));
    const double lambda = std::atan2(
        sinSigma * sinAlpha1,
        cosU1 * cosSigma - sinU1 * sinSigma * cosAlpha1);
    const double C = (f_ / 16.0) * cosSqAlpha * (4.0 + f_ * (4.0 - 3.0 * cosSqAlpha));
    const double L = lambda - (1.0 - C) * f_ * sinAlpha *
        (sigma + C * sinSigma *
            (cos2SigmaM + C * cosSigma *
                (-1.0 + 2.0 * cos2SigmaM * cos2SigmaM)));
    const double lon2 = start.longitude() + L;
    const double finalAzimuth = std::atan2(
        sinAlpha,
        -tmp);

    result.destination = Cartographic::fromRadians(lon2, lat2, start.height());
    result.finalAzimuthRadians = normalizeTwoPi(finalAzimuth);
    result.converged = iterations < 1000;
    return result;
}

const Ellipsoid& Ellipsoid::WGS84() {
    static const Ellipsoid wgs84(6378137.0, 6356752.3142451793);
    return wgs84;
}

} // namespace earth_engine
