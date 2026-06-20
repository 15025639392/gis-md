#include "TilePriorityMetrics.h"

#include <limits>

namespace earth_engine {

double TilePriorityMetrics::computeTilePriority(
    const Vec3& tileCenter,
    const Vec3& cameraPosition,
    const Vec3& cameraDirection,
    double distance) {
    Vec3 tileDirection = tileCenter - cameraPosition;
    const double magnitude = tileDirection.length();
    if (magnitude < 1e-5) {
        return std::numeric_limits<double>::max();
    }

    tileDirection = tileDirection / magnitude;
    return (1.0 - tileDirection.dot(cameraDirection)) * distance;
}

} // namespace earth_engine
