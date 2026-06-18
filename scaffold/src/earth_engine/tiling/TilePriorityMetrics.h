#pragma once

#include "../core/math/Vec3.h"

namespace earth_engine {

/// Cesium-native-aligned tile load priority metrics.
class TilePriorityMetrics {
public:
    /// View-direction-weighted tile priority.
    /// Lower = higher priority. Formula: (1 - dot(tileDir, viewDir)) * distance.
    static double computeTilePriority(const Vec3& tileCenter,
                                      const Vec3& cameraPosition,
                                      const Vec3& cameraDirection,
                                      double distance);
};

} // namespace earth_engine
