#pragma once

#include "../core/resources/FrameResourceBudget.h"

#include <algorithm>
#include <cstdint>

namespace earth_engine {

struct TileFrameResourceBudgetPlanInput {
    uint32_t maximumSimultaneousTileLoads = 20;
    double mainThreadLoadingTimeLimit = 0.0;
    bool interactionActive = false;
    bool resourceSmoothingActive = false;
};

class TileFrameResourceBudgetPlanner {
public:
    static FrameResourceBudgetConfig plan(
        const TileFrameResourceBudgetPlanInput& input) {
        FrameResourceBudgetConfig config;
        config.maxNetworkRequestsPerFrame =
            input.maximumSimultaneousTileLoads;
        config.maxTerrainContentNetworkRequestsPerFrame =
            input.maximumSimultaneousTileLoads;
        config.maxRasterNetworkRequestsPerFrame =
            std::max<uint32_t>(
                64u,
                input.maximumSimultaneousTileLoads * 4u);
        config.maxNetworkInflight = input.maximumSimultaneousTileLoads;
        config.maxTerrainContentNetworkInflight =
            input.maximumSimultaneousTileLoads;
        config.maxRasterNetworkInflight =
            config.maxRasterNetworkRequestsPerFrame;
        config.maxMainThreadFinalizesPerFrame =
            input.resourceSmoothingActive ? 1u
                                          : input.maximumSimultaneousTileLoads;
        config.maxRasterUploadsPerFrame =
            input.resourceSmoothingActive
                ? std::min<uint32_t>(
                      4u,
                      input.maximumSimultaneousTileLoads)
                : input.maximumSimultaneousTileLoads;
        config.mainThreadTimeMs = input.mainThreadLoadingTimeLimit;
        config.interactionActive = input.interactionActive;
        config.smoothingActive = input.resourceSmoothingActive;
        return config;
    }
};

} // namespace earth_engine
