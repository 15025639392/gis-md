#pragma once

#include "../core/math/Mat4.h"
#include "../core/math/Vec3.h"
#include "../scene/FrameState.h"

#include <vector>

namespace earth_engine {

struct TilesetTile;

struct TileSelectionInputSummary {
    std::vector<double> distances;
    double priority = 0.0;
    double screenSpaceError = 0.0;
};

struct TileSelectionInputMetrics {
    static double distanceToTile(const TilesetTile& tile,
                                 const Vec3& cameraPosition);

    static Vec3 tileCenter(const TilesetTile& tile);

    static double priorityForView(const Vec3& tileCenter,
                                  const Vec3& cameraPosition,
                                  const Vec3& cameraDirection,
                                  double distance);

    static double screenSpaceErrorForView(double geometricError,
                                          const Mat4& projectionMatrix,
                                          int viewportHeightPixels,
                                          double distance);

    static TileSelectionInputSummary summarizeForViews(
        const TilesetTile& tile,
        const std::vector<FrameState::SelectorView>& views);
};

} // namespace earth_engine
