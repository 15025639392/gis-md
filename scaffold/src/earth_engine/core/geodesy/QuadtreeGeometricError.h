#pragma once

#include "Ellipsoid.h"
#include "../math/Rectangle.h"

namespace earth_engine {

double calcQuadtreeMaxGeometricError(const Ellipsoid& ellipsoid);
double calcQuadtreeSkirtHeight(const Ellipsoid& ellipsoid,
                               const Rectangle& rectangle);
double calcLayerJsonTerrainGeometricError(const Ellipsoid& ellipsoid,
                                          const Rectangle& rectangle);

} // namespace earth_engine
