#pragma once

#include "TileFrameInteractionTracker.h"
#include "TileFrameResourceBudgetPlanner.h"
#include "../core/math/Vec3.h"
#include "../core/resources/FrameResourceBudget.h"
#include "../scene/FrameState.h"

#include <cstdint>

namespace earth_engine {

struct TileUpdateFrameContextOptions {
    uint32_t maximumSimultaneousTileLoads =
        TileFrameResourceBudgetPlanInput::kDefaultMaximumSimultaneousTileLoads;
    uint32_t maximumTransportActiveRequests =
        TileFrameResourceBudgetPlanInput::kDefaultMaximumTransportActiveRequests;
    double mainThreadLoadingTimeLimit = 0.0;
    double postInteractionResourceSmoothingSeconds = 0.0;
    bool cullRequestsWhileMoving = false;
    double cullRequestsWhileMovingMultiplier = 60.0;

    static TileUpdateFrameContextOptions fromTilesetFrame(
        uint32_t maximumSimultaneousTileLoads,
        uint32_t maximumTransportActiveRequests,
        double mainThreadLoadingTimeLimit,
        double postInteractionResourceSmoothingSeconds,
        bool cullRequestsWhileMoving,
        double cullRequestsWhileMovingMultiplier) {
        TileUpdateFrameContextOptions options;
        options.maximumSimultaneousTileLoads = maximumSimultaneousTileLoads;
        options.maximumTransportActiveRequests = maximumTransportActiveRequests;
        options.mainThreadLoadingTimeLimit = mainThreadLoadingTimeLimit;
        options.postInteractionResourceSmoothingSeconds =
            postInteractionResourceSmoothingSeconds;
        options.cullRequestsWhileMoving = cullRequestsWhileMoving;
        options.cullRequestsWhileMovingMultiplier =
            cullRequestsWhileMovingMultiplier;
        return options;
    }
};

struct TileUpdateFrameContext {
    TileFrameInteractionSnapshot interaction;
    FrameResourceBudgetConfig resourceBudgetConfig;
};

struct TileUpdateFrameContextBuilder {
    static TileUpdateFrameContext build(
        const FrameState& frameState,
        const Vec3& previousCameraPosition,
        double previousInteractionActiveTimeSeconds,
        const TileUpdateFrameContextOptions& options) {
        TileUpdateFrameContext context;
        context.interaction = TileFrameInteractionTracker::evaluate(
            frameState,
            previousCameraPosition,
            previousInteractionActiveTimeSeconds,
            options.postInteractionResourceSmoothingSeconds);
        TileFrameResourceBudgetPlanInput planInput =
            TileFrameResourceBudgetPlanInput::withTransportLimit(
                options.maximumSimultaneousTileLoads,
                options.maximumTransportActiveRequests,
                options.mainThreadLoadingTimeLimit,
                context.interaction.interactionActive,
                context.interaction.resourceSmoothingActive);
        planInput.cullRequestsWhileMoving = options.cullRequestsWhileMoving;
        planInput.cullRequestsWhileMovingMultiplier =
            options.cullRequestsWhileMovingMultiplier;
        planInput.cameraPositionDeltaMagnitude =
            context.interaction.cameraPositionDeltaMagnitude;
        context.resourceBudgetConfig =
            TileFrameResourceBudgetPlanner::plan(planInput);
        return context;
    }
};

} // namespace earth_engine
