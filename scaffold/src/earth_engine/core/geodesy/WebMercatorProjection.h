#pragma once

#include "Cartographic.h"
#include "Ellipsoid.h"
#include "../math/Rectangle.h"
#include "../math/Vec3.h"

#include <glm/ext/vector_double2.hpp>

namespace earth_engine {

class WebMercatorProjection {
public:
    explicit WebMercatorProjection(const Ellipsoid& ellipsoid);

    Vec3 project(const Cartographic& cartographic) const;
    Rectangle project(const Rectangle& rectangle) const;
    Cartographic unproject(const glm::dvec2& projectedCoordinates) const;
    Cartographic unproject(const Vec3& projectedCoordinates) const;
    Rectangle unproject(const Rectangle& rectangle) const;

    const Ellipsoid& ellipsoid() const { return ellipsoid_; }
    double semimajorAxis() const { return semimajorAxis_; }

    static double maximumLatitude();
    static Rectangle maximumGlobeRectangle();
    static Rectangle computeMaximumProjectedRectangle(const Ellipsoid& ellipsoid);
    static double mercatorAngleToGeodeticLatitude(double mercatorAngle);
    static double geodeticLatitudeToMercatorAngle(double latitude);

    bool operator==(const WebMercatorProjection& rhs) const {
        return ellipsoid_ == rhs.ellipsoid_;
    }
    bool operator!=(const WebMercatorProjection& rhs) const {
        return !(*this == rhs);
    }

private:
    Ellipsoid ellipsoid_;
    double semimajorAxis_ = 1.0;
    double oneOverSemimajorAxis_ = 1.0;
};

} // namespace earth_engine
