#include "QuadtreeGeometricError.h"

namespace earth_engine {

namespace {

constexpr double kTerrainMapQuality = 0.25;
constexpr double kTerrainMapWidth = 65.0;
constexpr double kSkirtHeightMultiplier = 5.0;
constexpr double kLayerJsonTerrainMultiplier = 8.0;

} // namespace

double calcQuadtreeMaxGeometricError(const Ellipsoid& ellipsoid) {
    return ellipsoid.maximumRadius() * kTerrainMapQuality / kTerrainMapWidth;
}

double calcQuadtreeSkirtHeight(const Ellipsoid& ellipsoid,
                               const Rectangle& rectangle) {
    return kSkirtHeightMultiplier *
           calcQuadtreeMaxGeometricError(ellipsoid) *
           rectangle.width();
}

double calcLayerJsonTerrainGeometricError(const Ellipsoid& ellipsoid,
                                          const Rectangle& rectangle) {
    return kLayerJsonTerrainMultiplier *
           calcQuadtreeMaxGeometricError(ellipsoid) *
           rectangle.width();
}

} // namespace earth_engine
