#include <gtest/gtest.h>

#include "earth_engine/core/math/Mat4.h"
#include "earth_engine/scene/FrameState.h"
#include "earth_engine/tiling/RasterMappedToTilesetTile.h"
#include "earth_engine/tiling/TileSelectionInputMetrics.h"
#include "earth_engine/tiling/TilesetTile.h"

#include <algorithm>
#include <glm/glm.hpp>
#include <vector>

using namespace earth_engine;

namespace {

Mat4 cesiumNativeLikeProjection(double yScale) {
    glm::dmat4 rawProjection(0.0);
    rawProjection[1][1] = yScale;
    rawProjection[2][3] = -1.0;
    rawProjection[3][2] = 1.0;
    return Mat4(rawProjection);
}

} // namespace

TEST(TileSelectionInputMetricsTest, ScreenSpaceErrorMatchesCesiumProjection) {
    const Mat4 projection = cesiumNativeLikeProjection(2.0);

    const double sse = TileSelectionInputMetrics::screenSpaceErrorForView(
        10.0,
        projection,
        800,
        1000.0);
    EXPECT_NEAR(sse, 8.0, 1e-12);

    const double closeSse = TileSelectionInputMetrics::screenSpaceErrorForView(
        10.0,
        projection,
        800,
        500.0);
    EXPECT_NEAR(closeSse, 16.0, 1e-12);

    const double tallViewportSse =
        TileSelectionInputMetrics::screenSpaceErrorForView(
            10.0,
            projection,
            1200,
            1000.0);
    EXPECT_NEAR(tallViewportSse, 12.0, 1e-12);
}

TEST(TileSelectionInputMetricsTest, ScreenSpaceErrorClampsZeroDistance) {
    const Mat4 projection = cesiumNativeLikeProjection(2.0);

    const double insideTileSse =
        TileSelectionInputMetrics::screenSpaceErrorForView(
            1.0,
            projection,
            800,
            0.0);
    const double clampedDistanceSse =
        TileSelectionInputMetrics::screenSpaceErrorForView(
            1.0,
            projection,
            800,
            1e-7);
    EXPECT_NEAR(insideTileSse, clampedDistanceSse, 1e-3);
}

TEST(TileSelectionInputMetricsTest, SummarizeUsesLargestPerViewSse) {
    TilesetTile tile(
        TileKey{"tileset", 0, 0, 0},
        Rectangle::fromDegrees(-1.0, -1.0, 1.0, 1.0));
    tile.refine = TileRefine::Replace;
    tile.geometricError = 10.0;

    std::vector<SelectorView> views(2);
    views[0].position = Vec3(6378137.0 + 1000.0, 0.0, 0.0);
    views[0].direction = Vec3(-1.0, 0.0, 0.0);
    views[0].projectionMatrix = cesiumNativeLikeProjection(2.0);
    views[0].viewportHeightPixels = 800;
    views[1].position = Vec3(6378137.0 + 500.0, 0.0, 0.0);
    views[1].direction = Vec3(-1.0, 0.0, 0.0);
    views[1].projectionMatrix = cesiumNativeLikeProjection(2.0);
    views[1].viewportHeightPixels = 800;

    std::vector<double> scratchDistances;
    const TileSelectionInputSummary summary =
        TileSelectionInputMetrics::summarizeForViews(
            tile, views, scratchDistances);
    ASSERT_EQ(scratchDistances.size(), views.size());

    const double firstViewSse =
        TileSelectionInputMetrics::screenSpaceErrorForView(
            tile.geometricError,
            views[0].projectionMatrix,
            views[0].viewportHeightPixels,
            scratchDistances[0]);
    const double secondViewSse =
        TileSelectionInputMetrics::screenSpaceErrorForView(
            tile.geometricError,
            views[1].projectionMatrix,
            views[1].viewportHeightPixels,
            scratchDistances[1]);
    EXPECT_NEAR(
        summary.screenSpaceError,
        std::max(firstViewSse, secondViewSse),
        1e-12);
}
