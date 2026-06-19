#pragma once

#include "TileRenderEntryCommandBuilder.h"
#include "TileRenderFrameMaintenance.h"

#include <cstddef>

namespace earth_engine {

struct TileRenderDebugLogInput {
    double selectedBuildMs = 0.0;
    double fadeBuildMs = 0.0;
    TileRenderFrameMaintenanceTimings maintenanceTimings;
    size_t selectedTileCount = 0;
    size_t fadingTileCount = 0;
    TileRenderEntryCommandStats renderStats;
    TileRenderEntryCommandStats selectedRenderStats;
    TileRenderEntryCommandStats fadingRenderStats;
    size_t commandCount = 0;
    bool interactionActive = false;
    bool resourceSmoothingActive = false;
    int synchronousRenderPrepCount = 0;
    int deferredRenderPrepCount = 0;
    int ancestorFallbackDrawCount = 0;
};

} // namespace earth_engine
