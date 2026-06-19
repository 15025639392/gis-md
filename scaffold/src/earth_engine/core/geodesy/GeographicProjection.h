#pragma once

#include "Cartographic.h"
#include "../math/Rectangle.h"
#include "../math/Vec3.h"

namespace earth_engine {

class Ellipsoid;

class GeographicProjection {
public:
    explicit GeographicProjection(const Ellipsoid& ellipsoid);

    Vec3 project(const Cartographic& cartographic) const;
    Rectangle project(const Rectangle& rectangle) const;
    Cartographic unproject(const Vec3& projectedCoordinates) const;
    Rectangle unproject(const Rectangle& rectangle) const;

    double semimajorAxis() const { return semimajorAxis_; }

    static Rectangle computeMaximumProjectedRectangle(const Ellipsoid& ellipsoid);

private:
    double semimajorAxis_ = 1.0;
    double oneOverSemimajorAxis_ = 1.0;
};

} // namespace earth_engine
