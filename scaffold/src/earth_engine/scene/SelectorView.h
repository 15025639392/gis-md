#pragma once

#include "../core/math/Mat4.h"
#include "../core/math/Vec3.h"
#include "Frustum.h"

namespace earth_engine {

/// cesium-native TilesetFrameState::frustums equivalent for selection.
/// Scene populates the default main-camera view. Empty means no frustums,
/// matching cesium-native's no-selection update path.
struct SelectorView {
    Vec3 position = Vec3::zero();
    Vec3 direction = Vec3::zero();
    Frustum frustum;
    Mat4 projectionMatrix;
    int viewportHeightPixels = 0;
};

} // namespace earth_engine
