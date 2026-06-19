#include "TileViewerRequestVolumePolicy.h"

#include "TileBoundsMetrics.h"

namespace earth_engine {

bool TileViewerRequestVolumePolicy::hasRequestVolume(
    const std::optional<TileBoundingVolume>& viewerRequestVolume) {
    return viewerRequestVolume.has_value();
}

bool TileViewerRequestVolumePolicy::containsPosition(
    const TileBoundingVolume& viewerRequestVolume,
    const Vec3& position) {
    return TileBoundsMetrics::boundingVolumeContainsPosition(
        viewerRequestVolume,
        position);
}

bool TileViewerRequestVolumePolicy::allowsAnyView(
    const std::optional<TileBoundingVolume>& viewerRequestVolume,
    const std::vector<SelectorView>& views) {
    if (!viewerRequestVolume) {
        return true;
    }

    for (const auto& view : views) {
        if (containsPosition(*viewerRequestVolume, view.position)) {
            return true;
        }
    }
    return false;
}

} // namespace earth_engine
