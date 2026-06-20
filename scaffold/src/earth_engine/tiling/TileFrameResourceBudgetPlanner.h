#pragma once

#include "../core/resources/FrameResourceBudget.h"

#include <algorithm>
#include <cstdint>
#include <limits>

namespace earth_engine {

struct TileFrameResourceBudgetPlanInput {
    static constexpr uint32_t kDefaultMaximumSimultaneousTileLoads = 20;
    static constexpr uint32_t kDefaultMaximumTransportActiveRequests = 20;

    uint32_t maximumSimultaneousTileLoads =
        kDefaultMaximumSimultaneousTileLoads;
    uint32_t maximumTransportActiveRequests =
        kDefaultMaximumTransportActiveRequests;
    double mainThreadLoadingTimeLimit = 0.0;
    bool interactionActive = false;
    bool resourceSmoothingActive = false;

    static TileFrameResourceBudgetPlanInput withDefaultTransport(
        uint32_t maximumSimultaneousTileLoads,
        double mainThreadLoadingTimeLimit,
        bool interactionActive,
        bool resourceSmoothingActive) {
        TileFrameResourceBudgetPlanInput input;
        input.maximumSimultaneousTileLoads = maximumSimultaneousTileLoads;
        input.mainThreadLoadingTimeLimit = mainThreadLoadingTimeLimit;
        input.interactionActive = interactionActive;
        input.resourceSmoothingActive = resourceSmoothingActive;
        return input;
    }

    static TileFrameResourceBudgetPlanInput withTransportLimit(
        uint32_t maximumSimultaneousTileLoads,
        uint32_t maximumTransportActiveRequests,
        double mainThreadLoadingTimeLimit,
        bool interactionActive,
        bool resourceSmoothingActive) {
        TileFrameResourceBudgetPlanInput input =
            withDefaultTransport(
                maximumSimultaneousTileLoads,
                mainThreadLoadingTimeLimit,
                interactionActive,
                resourceSmoothingActive);
        input.maximumTransportActiveRequests = maximumTransportActiveRequests;
        return input;
    }
};

class TileFrameResourceBudgetPlanner {
public:
    static uint32_t rasterNetworkRequestLimit(
        uint32_t maximumSimultaneousTileLoads,
        uint32_t maximumTransportActiveRequests) {
        constexpr uint32_t kMinimumRectangleFanoutBudget = 32u;
        const uint32_t rectangleFanoutBudget = std::max(
            kMinimumRectangleFanoutBudget,
            maximumSimultaneousTileLoads);
        if (maximumTransportActiveRequests == 0) {
            return rectangleFanoutBudget;
        }
        return std::min(rectangleFanoutBudget, maximumTransportActiveRequests);
    }

    static FrameResourceBudgetConfig plan(
        const TileFrameResourceBudgetPlanInput& input) {
        FrameResourceBudgetConfig config;
        const uint32_t rasterNetworkLimit =
            rasterNetworkRequestLimit(
                input.maximumSimultaneousTileLoads,
                input.maximumTransportActiveRequests);
        config.maxNetworkRequestsPerFrame =
            input.maximumSimultaneousTileLoads;
        config.maxTerrainContentNetworkRequestsPerFrame =
            input.maximumSimultaneousTileLoads;
        config.maxRasterNetworkRequestsPerFrame = rasterNetworkLimit;
        config.maxNetworkInflight = input.maximumSimultaneousTileLoads;
        config.maxTerrainContentNetworkInflight =
            input.maximumSimultaneousTileLoads;
        config.maxRasterNetworkInflight = rasterNetworkLimit;
        config.maxMainThreadFinalizesPerFrame =
            input.resourceSmoothingActive ? 1u
                                          : input.maximumSimultaneousTileLoads == 0
                                              ? std::numeric_limits<uint32_t>::max()
                                              : input.maximumSimultaneousTileLoads;
        config.maxTerminalStateTransitionsPerFrame =
            input.resourceSmoothingActive
                ? std::max<uint32_t>(1u, input.maximumSimultaneousTileLoads / 2u)
                : input.maximumSimultaneousTileLoads == 0
                    ? std::numeric_limits<uint32_t>::max()
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
