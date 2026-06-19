#pragma once

#include "Cartographic.h"
#include "Ellipsoid.h"
#include "GeographicProjection.h"
#include "WebMercatorProjection.h"
#include "../math/Rectangle.h"
#include "../math/Vec3.h"

#include <glm/ext/vector_double2.hpp>
#include <variant>

namespace earth_engine {

using Projection = std::variant<GeographicProjection, WebMercatorProjection>;

Vec3 projectPosition(const Projection& projection, const Cartographic& position);
Cartographic unprojectPosition(const Projection& projection, const Vec3& position);

Rectangle projectRectangleSimple(const Projection& projection,
                                 const Rectangle& rectangle);
Rectangle unprojectRectangleSimple(const Projection& projection,
                                   const Rectangle& rectangle);

glm::dvec2 computeProjectedRectangleSize(const Projection& projection,
                                         const Rectangle& rectangle,
                                         double maxHeight,
                                         const Ellipsoid& ellipsoid);

} // namespace earth_engine
