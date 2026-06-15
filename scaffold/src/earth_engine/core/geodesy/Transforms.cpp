#include "Transforms.h"
#include "Ellipsoid.h"
#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <cmath>

namespace earth_engine {

namespace {
    constexpr double kEpsilon14 = 1e-14;

    bool equalsEpsilon(double left, double right, double epsilon) {
        return std::abs(left - right) <= epsilon;
    }

    bool equalsZero(const Vec3& value, double epsilon) {
        return equalsEpsilon(value.x(), 0.0, epsilon) &&
               equalsEpsilon(value.y(), 0.0, epsilon) &&
               equalsEpsilon(value.z(), 0.0, epsilon);
    }

    double sign(double value) {
        if (value == 0.0 || value != value) {
            return value;
        }
        return value > 0.0 ? 1.0 : -1.0;
    }
}

double Transforms::toRadians(double deg) {
    return deg * glm::pi<double>() / 180.0;
}

double Transforms::toDegrees(double rad) {
    return rad * 180.0 / glm::pi<double>();
}

Mat4 Transforms::eastNorthUpToFixedFrame(const Vec3& originEcef) {
    return eastNorthUpToFixedFrame(originEcef, Ellipsoid::WGS84());
}

Mat4 Transforms::eastNorthUpToFixedFrame(const Vec3& originEcef,
                                         const Ellipsoid& ellipsoid) {
    const glm::dvec3 origin = originEcef.raw();

    if (equalsZero(originEcef, kEpsilon14)) {
        return Mat4(glm::dmat4(
            glm::dvec4(0.0, 1.0, 0.0, 0.0),
            glm::dvec4(-1.0, 0.0, 0.0, 0.0),
            glm::dvec4(0.0, 0.0, 1.0, 0.0),
            glm::dvec4(origin, 1.0)));
    }

    if (equalsEpsilon(originEcef.x(), 0.0, kEpsilon14) &&
        equalsEpsilon(originEcef.y(), 0.0, kEpsilon14)) {
        const double poleSign = sign(originEcef.z());
        return Mat4(glm::dmat4(
            glm::dvec4(0.0, 1.0, 0.0, 0.0),
            glm::dvec4(-poleSign, 0.0, 0.0, 0.0),
            glm::dvec4(0.0, 0.0, poleSign, 0.0),
            glm::dvec4(origin, 1.0)));
    }

    const Vec3 upVec = ellipsoid.geodeticSurfaceNormal(originEcef);
    const glm::dvec3 up = upVec.raw();
    const glm::dvec3 east =
        glm::normalize(glm::dvec3(-origin.y, origin.x, 0.0));
    const glm::dvec3 north = glm::cross(up, east);

    return Mat4(glm::dmat4(
        glm::dvec4(east, 0.0),
        glm::dvec4(north, 0.0),
        glm::dvec4(up, 0.0),
        glm::dvec4(origin, 1.0)));
}

Mat4 Transforms::ecefToEnu(const Cartographic& origin) {
    Vec3 originEcef = Ellipsoid::WGS84().cartographicToCartesian(origin);
    return eastNorthUpToFixedFrame(originEcef).inverse();
}

Mat4 Transforms::enuToEcef(const Cartographic& origin) {
    Vec3 originEcef = Ellipsoid::WGS84().cartographicToCartesian(origin);
    return eastNorthUpToFixedFrame(originEcef);
}

} // namespace earth_engine
