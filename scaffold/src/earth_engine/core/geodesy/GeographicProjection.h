#pragma once

#include "Cartographic.h"
#include "../math/Vec3.h"

namespace earth_engine {

class Ellipsoid;

class GeographicProjection {
public:
    explicit GeographicProjection(const Ellipsoid& ellipsoid);

    Vec3 project(const Cartographic& cartographic) const;
    Cartographic unproject(const Vec3& projectedCoordinates) const;

    double semimajorAxis() const { return semimajorAxis_; }

private:
    double semimajorAxis_ = 1.0;
    double oneOverSemimajorAxis_ = 1.0;
};

} // namespace earth_engine
