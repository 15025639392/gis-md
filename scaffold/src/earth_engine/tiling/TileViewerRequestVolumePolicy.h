#pragma once

#include "../core/math/Vec3.h"
#include "../scene/FrameState.h"

#include <optional>
#include <vector>

namespace earth_engine {

struct TileBoundingVolume;

struct TileViewerRequestVolumePolicy {
    static bool hasRequestVolume(
        const std::optional<TileBoundingVolume>& viewerRequestVolume);

    static bool containsPosition(
        const TileBoundingVolume& viewerRequestVolume,
        const Vec3& position);

    static bool allowsAnyView(
        const std::optional<TileBoundingVolume>& viewerRequestVolume,
        const std::vector<SelectorView>& views);
};

} // namespace earth_engine
