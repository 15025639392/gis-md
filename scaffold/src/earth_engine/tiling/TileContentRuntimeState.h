#pragma once

#include "TileLoadState.h"
#include "TileRenderContentState.h"

namespace earth_engine {

/// Cross-frame content lifecycle state for a tile.
///
/// This groups the terrain/glTF render resources with the load/content
/// classification that governs upload, unload and renderability decisions.
struct TileContentRuntimeState {
    TileRenderContentState renderContent;
    TileLoadState loadState = TileLoadState::Unloaded;
    TileContentKind contentKind = TileContentKind::Unknown;

    /// cesium-native UpsampledQuadtreeNode equivalent. The tile is not
    /// requestable; its render content is derived from an ancestor tile.
    bool upsampledFromParent = false;
};

} // namespace earth_engine
