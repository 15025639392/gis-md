#pragma once

#include "TileFrameResourceBudgetPlanner.h"
#include "../core/resources/FrameResourceBudget.h"

#include <cstdint>

namespace earth_engine {

struct TileFrameBudgetFallback {
    static FrameResourceBudgetConfig requestConfig(
        uint32_t maximumSimultaneousTileLoads,
        double mainThreadLoadingTimeLimit) {
        return TileFrameResourceBudgetPlanner::plan(
            TileFrameResourceBudgetPlanInput::withDefaultTransport(
                maximumSimultaneousTileLoads,
                mainThreadLoadingTimeLimit,
                false,
                false));
    }

    static FrameResourceBudgetConfig uploadConfig(
        uint32_t maximumSimultaneousTileLoads,
        double mainThreadLoadingTimeLimit,
        bool interactionActive,
        bool resourceSmoothingActive,
        uint32_t smoothedMainThreadUploadLimit) {
        FrameResourceBudgetConfig config;
        config.maxMainThreadFinalizesPerFrame =
            resourceSmoothingActive
                ? smoothedMainThreadUploadLimit
                : maximumSimultaneousTileLoads;
        config.mainThreadTimeMs = mainThreadLoadingTimeLimit;
        config.interactionActive = interactionActive;
        config.smoothingActive = resourceSmoothingActive;
        return config;
    }

};

} // namespace earth_engine
