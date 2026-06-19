#pragma once

#include "TileFrameDebugLogFormatter.h"
#include "TileRasterOverlaySignature.h"
#include "TileSelectionReuseState.h"
#include "TileUpdateFrameContextBuilder.h"
#include "TileUpdateSelectionWorkRunner.h"
#include "TileUpdateUploadRunner.h"
#include "../core/math/Vec3.h"
#include "../core/resources/FrameResourceBudget.h"
#include "../layers/ActivatedRasterOverlay.h"
#include "../renderer/RenderDevice.h"
#include "../scene/FrameState.h"

#include <cstdint>
#include <vector>

namespace earth_engine {

struct TileFrameWorkState {
    bool& cameraMoving;
    bool& interactionActiveForFrame;
    bool& resourceSmoothingActiveForFrame;
    double& lastInteractionActiveTimeSeconds;
    Vec3& lastCameraPosition;
    Vec3& lastCameraDirection;
};

struct TileFrameWorkInput {
    TilePlan& tilePlan;
    TileLoadQueue& loadQueue;
    TileSelectionCounters& selectionCounters;
    TileSelectionReuseState& selectionReuseState;
    std::vector<ActivatedRasterOverlay*>& rasterOverlays;
    FrameResourceBudget& frameResourceBudget;
    RenderDevice* device = nullptr;
    const FrameState& frameState;
    uint64_t resourceRevision = 0;
    uint32_t maximumSimultaneousTileLoads = 20;
    uint32_t maximumTransportActiveRequests =
        TileFrameResourceBudgetPlanInput::kDefaultMaximumTransportActiveRequests;
    double mainThreadLoadingTimeLimit = 0.0;
    double postInteractionResourceSmoothingSeconds = 0.0;
    double maximumScreenSpaceError = 16.0;
};

struct TileFrameWorkResult {
    TileUpdateUploadRunResult uploadWork;
    TileUpdateSelectionWorkResult selectionWork;
    bool interactionActive = false;
    bool resourceSmoothingActive = false;
};

class TileFrameWorkCoordinator {
public:
    template <typename ProcessPendingUploadsFn,
              typename MarkResourcesDirtyFn,
              typename HasTilesetPendingWorkFn,
              typename RefreshTilePlanRenderEntriesFn,
              typename SelectTilesFn,
              typename EnsureTileFn,
              typename RequestMissingTilesFn>
    static TileFrameWorkResult run(
        TileFrameWorkInput input,
        TileFrameWorkState state,
        ProcessPendingUploadsFn&& processPendingUploads,
        MarkResourcesDirtyFn&& markResourcesDirty,
        HasTilesetPendingWorkFn&& hasTilesetPendingWork,
        RefreshTilePlanRenderEntriesFn&& refreshTilePlanRenderEntries,
        SelectTilesFn&& selectTiles,
        EnsureTileFn&& ensureTile,
        RequestMissingTilesFn&& requestMissingTiles) {
        TileFrameWorkResult result;

        const TileUpdateFrameContext frameContext =
            TileUpdateFrameContextBuilder::build(
                input.frameState,
                state.lastCameraPosition,
                state.lastInteractionActiveTimeSeconds,
                TileUpdateFrameContextOptions::fromTilesetFrame(
                    input.maximumSimultaneousTileLoads,
                    input.maximumTransportActiveRequests,
                    input.mainThreadLoadingTimeLimit,
                    input.postInteractionResourceSmoothingSeconds));
        const TileFrameInteractionSnapshot& interactionSnapshot =
            frameContext.interaction;
        state.cameraMoving = interactionSnapshot.cameraMoving;
        state.lastCameraPosition = interactionSnapshot.cameraPosition;
        state.lastCameraDirection = interactionSnapshot.cameraDirection;
        state.interactionActiveForFrame =
            interactionSnapshot.interactionActive;
        state.lastInteractionActiveTimeSeconds =
            interactionSnapshot.lastInteractionActiveTimeSeconds;
        state.resourceSmoothingActiveForFrame =
            interactionSnapshot.resourceSmoothingActive;
        result.interactionActive = interactionSnapshot.interactionActive;
        result.resourceSmoothingActive =
            interactionSnapshot.resourceSmoothingActive;

        input.frameResourceBudget.beginFrame(
            input.frameState.frameId,
            frameContext.resourceBudgetConfig);

        result.uploadWork = TileUpdateUploadRunner::run(
            TileUpdateUploadRunInput{
                input.rasterOverlays,
                input.frameResourceBudget,
                result.interactionActive,
                result.resourceSmoothingActive},
            processPendingUploads,
            markResourcesDirty);

        const uint64_t currentResourceRevision =
            TileRasterOverlaySignature::selectionResourceRevision(
                input.resourceRevision,
                input.rasterOverlays);
        const uint64_t currentOverlaySignature =
            TileRasterOverlaySignature::configuration(input.rasterOverlays);
        const TileSelectionReuseClassification reuseClassification =
            input.selectionReuseState.classifyReuseWithReason(
                input.frameState,
                currentResourceRevision,
                currentOverlaySignature,
                result.resourceSmoothingActive,
                !input.tilePlan.tilesFadingOut.empty() ||
                    input.tilePlan.fadingNodeCount > 0,
                hasTilesetPendingWork(),
                TileRasterOverlaySignature::hasPendingWork(
                    input.rasterOverlays));
        const TileSelectionReuseMode reuseMode = reuseClassification.mode;
        const bool reusedSelection =
            reuseMode != TileSelectionReuseMode::None;

        result.selectionWork = TileUpdateSelectionWorkRunner::run(
            TileUpdateSelectionWorkInput{
                input.tilePlan,
                input.loadQueue,
                input.selectionCounters,
                input.selectionReuseState,
                input.rasterOverlays,
                input.frameResourceBudget,
                input.device,
                input.frameState,
                currentResourceRevision,
                currentOverlaySignature,
                reuseMode,
                reuseClassification.rejectReason,
                reusedSelection,
                input.maximumScreenSpaceError},
            refreshTilePlanRenderEntries,
            selectTiles,
            ensureTile,
            requestMissingTiles);

        return result;
    }
};

} // namespace earth_engine
