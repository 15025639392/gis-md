#include "TileSelectionMetrics.h"

#include <algorithm>
#include <cmath>

namespace earth_engine {

double TileSelectionMetrics::computeFogDensity(
    const std::vector<FogDensityAtHeight>& fogDensityTable,
    double cameraHeightMeters) {
    if (fogDensityTable.empty()) {
        return 0.0;
    }

    // cesium-native: binary search for the entry >= camera height.
    auto nextIt = std::lower_bound(
        fogDensityTable.begin(), fogDensityTable.end(), cameraHeightMeters,
        [](const FogDensityAtHeight& entry, double height) {
            return entry.cameraHeight < height;
        });

    if (nextIt == fogDensityTable.end()) {
        return fogDensityTable.back().fogDensity;
    }
    if (nextIt == fogDensityTable.begin()) {
        return nextIt->fogDensity;
    }

    auto prevIt = nextIt - 1;
    double t = (cameraHeightMeters - prevIt->cameraHeight) /
               (nextIt->cameraHeight - prevIt->cameraHeight);
    t = std::max(0.0, std::min(1.0, t));

    return prevIt->fogDensity + t * (nextIt->fogDensity - prevIt->fogDensity);
}

bool TileSelectionMetrics::isVisibleInFog(double distance, double fogDensity) {
    // cesium-native: exp(-(distance * fogDensity)^2) > 0.0.
    if (fogDensity <= 0.0) return true;
    const double fogScalar = distance * fogDensity;
    return std::exp(-(fogScalar * fogScalar)) > 0.0;
}

} // namespace earth_engine
