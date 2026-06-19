#pragma once

#include "Cartographic.h"
#include "../math/Vec3.h"

namespace earth_engine {

class Ellipsoid;

class WebMercatorProjection {
public:
    explicit WebMercatorProjection(const Ellipsoid& ellipsoid);

    Vec3 project(const Cartographic& cartographic) const;
    Cartographic unproject(const Vec3& projectedCoordinates) const;

    double semimajorAxis() const { return semimajorAxis_; }

    static double maximumLatitude();
    static double mercatorAngleToGeodeticLatitude(double mercatorAngle);
    static double geodeticLatitudeToMercatorAngle(double latitude);

private:
    double semimajorAxis_ = 1.0;
    double oneOverSemimajorAxis_ = 1.0;
};

} // namespace earth_engine
