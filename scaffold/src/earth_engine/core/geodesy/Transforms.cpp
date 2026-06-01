#include "Transforms.h"
#include "Ellipsoid.h"
#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <cmath>

namespace earth_engine {

double Transforms::toRadians(double deg) {
    return deg * glm::pi<double>() / 180.0;
}

double Transforms::toDegrees(double rad) {
    return rad * 180.0 / glm::pi<double>();
}

Mat4 Transforms::ecefToEnu(const Cartographic& origin) {
    double lng = origin.longitude();
    double lat = origin.latitude();
    Vec3 originEcef = Ellipsoid::WGS84().cartographicToCartesian(origin);

    double sinLng = std::sin(lng);
    double cosLng = std::cos(lng);
    double sinLat = std::sin(lat);
    double cosLat = std::cos(lat);

    // ENU axes expressed in ECEF coordinates.
    // East  = (-sinLng,            cosLng,          0)
    // North = (-sinLat*cosLng,    -sinLat*sinLng,  cosLat)
    // Up    = ( cosLat*cosLng,     cosLat*sinLng,  sinLat)
    //
    // GLM stores columns, so rows below encode dot(axis, point - origin).
    glm::dmat4 m(1.0);
    const glm::dvec3 east(-sinLng, cosLng, 0.0);
    const glm::dvec3 north(-sinLat * cosLng, -sinLat * sinLng, cosLat);
    const glm::dvec3 up(cosLat * cosLng, cosLat * sinLng, sinLat);
    const glm::dvec3 originRaw = originEcef.raw();

    m[0][0] = east.x;
    m[1][0] = east.y;
    m[2][0] = east.z;
    m[3][0] = -glm::dot(east, originRaw);

    m[0][1] = north.x;
    m[1][1] = north.y;
    m[2][1] = north.z;
    m[3][1] = -glm::dot(north, originRaw);

    m[0][2] = up.x;
    m[1][2] = up.y;
    m[2][2] = up.z;
    m[3][2] = -glm::dot(up, originRaw);

    return Mat4(m);
}

Mat4 Transforms::enuToEcef(const Cartographic& origin) {
    return ecefToEnu(origin).inverse();
}

} // namespace earth_engine
