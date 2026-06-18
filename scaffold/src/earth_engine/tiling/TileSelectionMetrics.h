#pragma once

#include <vector>

namespace earth_engine {

/// cesium-native TilesetOptions::FogDensityAtHeight equivalent.
struct FogDensityAtHeight {
    double cameraHeight;  // meters above ellipsoid
    double fogDensity;    // density value
};

/// Pure selection-stage metrics used by tileset traversal.
class TileSelectionMetrics {
public:
    /// cesium-native: compute fog density from camera height using density table.
    static double computeFogDensity(
        const std::vector<FogDensityAtHeight>& fogDensityTable,
        double cameraHeightMeters);

    /// cesium-native: exponential fog visibility test.
    static bool isVisibleInFog(double distance, double fogDensity);
};

} // namespace earth_engine
