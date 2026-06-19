#pragma once

#include "TileSelectionCounters.h"
#include "TileSelectionReusePolicy.h"

#include <cstddef>

namespace earth_engine {

struct TileUpdateDebugLogInput {
    size_t renderTileCount = 0;
    size_t loadRequestCount = 0;
    double selectorMs = 0.0;
    double prefetchMs = 0.0;
    double requestMs = 0.0;
    double terrainUploadMs = 0.0;
    double rasterUploadMs = 0.0;
    size_t terrainCacheSize = 0;
    size_t pendingRequestCount = 0;
    TileSelectionCounters selectionCounters;
    TileSelectionReuseMode reuseMode = TileSelectionReuseMode::None;
    TileSelectionReuseRejectReason reuseRejectReason =
        TileSelectionReuseRejectReason::None;
    bool reusedSelection = false;
    int rasterUploadsProcessed = 0;
    bool interactionActive = false;
    bool resourceSmoothingActive = false;
};

} // namespace earth_engine
