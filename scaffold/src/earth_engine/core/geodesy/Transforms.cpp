#include "Transforms.h"
#include "Ellipsoid.h"
#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <cmath>
#include <limits>

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

    const Mat4& identityTransform() {
        static const Mat4 identity = Mat4::identity();
        return identity;
    }
}

const Mat4& Transforms::Y_UP_TO_Z_UP() {
    static const Mat4 transform(glm::dmat4(
        glm::dvec4(1.0, 0.0, 0.0, 0.0),
        glm::dvec4(0.0, 0.0, 1.0, 0.0),
        glm::dvec4(0.0, -1.0, 0.0, 0.0),
        glm::dvec4(0.0, 0.0, 0.0, 1.0)));
    return transform;
}

const Mat4& Transforms::Z_UP_TO_Y_UP() {
    static const Mat4 transform(glm::dmat4(
        glm::dvec4(1.0, 0.0, 0.0, 0.0),
        glm::dvec4(0.0, 0.0, -1.0, 0.0),
        glm::dvec4(0.0, 1.0, 0.0, 0.0),
        glm::dvec4(0.0, 0.0, 0.0, 1.0)));
    return transform;
}

const Mat4& Transforms::X_UP_TO_Z_UP() {
    static const Mat4 transform(glm::dmat4(
        glm::dvec4(0.0, 0.0, 1.0, 0.0),
        glm::dvec4(0.0, 1.0, 0.0, 0.0),
        glm::dvec4(-1.0, 0.0, 0.0, 0.0),
        glm::dvec4(0.0, 0.0, 0.0, 1.0)));
    return transform;
}

const Mat4& Transforms::Z_UP_TO_X_UP() {
    static const Mat4 transform(glm::dmat4(
        glm::dvec4(0.0, 0.0, -1.0, 0.0),
        glm::dvec4(0.0, 1.0, 0.0, 0.0),
        glm::dvec4(1.0, 0.0, 0.0, 0.0),
        glm::dvec4(0.0, 0.0, 0.0, 1.0)));
    return transform;
}

const Mat4& Transforms::X_UP_TO_Y_UP() {
    static const Mat4 transform(glm::dmat4(
        glm::dvec4(0.0, 1.0, 0.0, 0.0),
        glm::dvec4(-1.0, 0.0, 0.0, 0.0),
        glm::dvec4(0.0, 0.0, 1.0, 0.0),
        glm::dvec4(0.0, 0.0, 0.0, 1.0)));
    return transform;
}

const Mat4& Transforms::Y_UP_TO_X_UP() {
    static const Mat4 transform(glm::dmat4(
        glm::dvec4(0.0, -1.0, 0.0, 0.0),
        glm::dvec4(1.0, 0.0, 0.0, 0.0),
        glm::dvec4(0.0, 0.0, 1.0, 0.0),
        glm::dvec4(0.0, 0.0, 0.0, 1.0)));
    return transform;
}

const Mat4& Transforms::getUpAxisTransform(UpAxis from, UpAxis to) {
    switch (from) {
        case UpAxis::X:
            switch (to) {
                case UpAxis::X: return identityTransform();
                case UpAxis::Y: return X_UP_TO_Y_UP();
                case UpAxis::Z: return X_UP_TO_Z_UP();
            }
            break;
        case UpAxis::Y:
            switch (to) {
                case UpAxis::X: return Y_UP_TO_X_UP();
                case UpAxis::Y: return identityTransform();
                case UpAxis::Z: return Y_UP_TO_Z_UP();
            }
            break;
        case UpAxis::Z:
            switch (to) {
                case UpAxis::X: return Z_UP_TO_X_UP();
                case UpAxis::Y: return Z_UP_TO_Y_UP();
                case UpAxis::Z: return identityTransform();
            }
            break;
    }
    return identityTransform();
}

double Transforms::toRadians(double deg) {
    return deg * glm::pi<double>() / 180.0;
}

double Transforms::toDegrees(double rad) {
    return rad * 180.0 / glm::pi<double>();
}

Mat4 Transforms::createPerspectiveMatrix(double horizontalFovRadians,
                                         double verticalFovRadians,
                                         double zNear,
                                         double zFar) {
    return Mat4(glm::dmat4(
        glm::dvec4(1.0 / std::tan(horizontalFovRadians * 0.5), 0.0, 0.0, 0.0),
        glm::dvec4(0.0, -1.0 / std::tan(verticalFovRadians * 0.5), 0.0, 0.0),
        glm::dvec4(
            0.0,
            0.0,
            zFar == std::numeric_limits<double>::infinity()
                ? 0.0
                : zNear / (zFar - zNear),
            -1.0),
        glm::dvec4(
            0.0,
            0.0,
            zFar == std::numeric_limits<double>::infinity()
                ? zNear
                : zNear * zFar / (zFar - zNear),
            0.0)));
}

Mat4 Transforms::createPerspectiveMatrix(double left,
                                         double right,
                                         double bottom,
                                         double top,
                                         double zNear,
                                         double zFar) {
    const double m22 = zFar == std::numeric_limits<double>::infinity()
        ? 0.0
        : zNear / (zFar - zNear);
    const double m32 = zFar == std::numeric_limits<double>::infinity()
        ? zNear
        : zNear * zFar / (zFar - zNear);

    return Mat4(glm::dmat4(
        glm::dvec4(2.0 * zNear / (right - left), 0.0, 0.0, 0.0),
        glm::dvec4(0.0, 2.0 * zNear / (bottom - top), 0.0, 0.0),
        glm::dvec4(
            (right + left) / (right - left),
            (bottom + top) / (bottom - top),
            m22,
            -1.0),
        glm::dvec4(0.0, 0.0, m32, 0.0)));
}

Mat4 Transforms::createOrthographicMatrix(double left,
                                          double right,
                                          double bottom,
                                          double top,
                                          double zNear,
                                          double zFar) {
    const double m22 = zFar == std::numeric_limits<double>::infinity()
        ? 0.0
        : 1.0 / (zFar - zNear);
    const double m32 = zFar == std::numeric_limits<double>::infinity()
        ? 1.0
        : zFar / (zFar - zNear);

    return Mat4(glm::dmat4(
        glm::dvec4(2.0 / (right - left), 0.0, 0.0, 0.0),
        glm::dvec4(0.0, 2.0 / (bottom - top), 0.0, 0.0),
        glm::dvec4(0.0, 0.0, m22, 0.0),
        glm::dvec4(
            -(right + left) / (right - left),
            -(bottom + top) / (bottom - top),
            m32,
            1.0)));
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
