#include "TilePriorityMetrics.h"

#include <algorithm>

namespace earth_engine {

double TilePriorityMetrics::computeTilePriority(
    const Vec3& tileCenter,
    const Vec3& cameraPosition,
    const Vec3& cameraDirection,
    double distance) {
    Vec3 tileDirection = tileCenter - cameraPosition;
    const double magnitude = tileDirection.length();
    if (magnitude < 1e-5) return distance;

    tileDirection = tileDirection / magnitude;
    double viewDot = tileDirection.dot(cameraDirection);
    viewDot = std::max(-1.0, std::min(1.0, viewDot));
    return (1.0 - viewDot) * distance;
}

} // namespace earth_engine
