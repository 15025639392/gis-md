#include "TileSelectionInputMetrics.h"

#include "RasterMappedToTilesetTile.h"
#include "TileBoundsMetrics.h"
#include "TilePriorityMetrics.h"
#include "TilesetTile.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <glm/glm.hpp>

namespace earth_engine {

double TileSelectionInputMetrics::distanceToTile(
    const TilesetTile& tile,
    const Vec3& cameraPosition) {
    return TileBoundsMetrics::approximateDistanceToTileBounds(
        tile,
        cameraPosition);
}

Vec3 TileSelectionInputMetrics::tileCenter(const TilesetTile& tile) {
    return tile.boundingVolume
        ? TileBoundsMetrics::boundingVolumeCenter(*tile.boundingVolume)
        : TileBoundsMetrics::tileBoundsCenter(tile.bounds);
}

double TileSelectionInputMetrics::priorityForView(
    const Vec3& tileCenter,
    const Vec3& cameraPosition,
    const Vec3& cameraDirection,
    double distance) {
    return TilePriorityMetrics::computeTilePriority(
        tileCenter,
        cameraPosition,
        cameraDirection,
        distance);
}

double TileSelectionInputMetrics::screenSpaceErrorForView(
    double geometricError,
    const Mat4& projectionMatrix,
    int viewportHeightPixels,
    double distance) {
    if (geometricError <= 0.0) return 0.0;

    const double safeDistance = std::max(distance, 1e-7);
    const double viewportHeight =
        std::max(1.0, static_cast<double>(viewportHeightPixels));
    const glm::dmat4& projection = projectionMatrix.raw();
    glm::dvec4 centerNdc =
        projection * glm::dvec4(0.0, 0.0, -safeDistance, 1.0);
    centerNdc /= centerNdc.w;
    glm::dvec4 errorOffsetNdc =
        projection * glm::dvec4(0.0, geometricError, -safeDistance, 1.0);
    errorOffsetNdc /= errorOffsetNdc.w;
    return std::abs((errorOffsetNdc - centerNdc).y) * viewportHeight * 0.5;
}

TileSelectionInputSummary TileSelectionInputMetrics::summarizeForViews(
    const TilesetTile& tile,
    const std::vector<FrameState::SelectorView>& views) {
    TileSelectionInputSummary summary;
    summary.distances.reserve(views.size());
    for (const auto& view : views) {
        summary.distances.push_back(distanceToTile(tile, view.position));
    }

    const Vec3 center = tileCenter(tile);
    summary.priority = std::numeric_limits<double>::max();
    const size_t count = std::min(views.size(), summary.distances.size());
    for (size_t i = 0; i < count; ++i) {
        const auto& view = views[i];
        summary.priority = std::min(
            summary.priority,
            priorityForView(
                center,
                view.position,
                view.direction,
                summary.distances[i]));
        summary.screenSpaceError = std::max(
            summary.screenSpaceError,
            screenSpaceErrorForView(
                tile.geometricError,
                view.projectionMatrix,
                view.viewportHeightPixels,
                summary.distances[i]));
    }

    return summary;
}

} // namespace earth_engine
