#pragma once

#include "TileSelectionCullingPolicy.h"
#include "TileSelectionInputMetrics.h"
#include "TileSelectionVisibilitySampler.h"

#include "../scene/FrameState.h"

#include <vector>

namespace earth_engine {

struct TilesetTile;

struct TileSelectionVisitPreparationOptions {
    bool enableFrustumCulling = true;
    bool enableFogCulling = true;
    bool preloadSiblings = false;
    bool forbidHoles = false;
    bool enforceCulledScreenSpaceError = true;
    double maximumScreenSpaceError = 16.0;
    double culledScreenSpaceError = 64.0;
};

struct TileSelectionVisitPreparationResult {
    TileSelectionVisibilitySample visibilitySample;
    TileSelectionInputSummary inputSummary;
    TileSelectionCullResult cullResult;
    TileSelectionCullLoadPlan culledLoadPlan;
    bool viewerRequestVolumeAllowed = true;
    bool meetsScreenSpaceError = false;
};

enum class TileSelectionVisitEarlyExitCounter {
    None,
    FrustumCulled,
    FogCulled
};

enum class TileSelectionVisitEarlyExitReason {
    None,
    Culled,
    ViewerRequestVolume
};

struct TileSelectionVisitEarlyExitPlan {
    bool shouldExit = false;
    TileSelectionVisitEarlyExitReason reason =
        TileSelectionVisitEarlyExitReason::None;
    TileSelectionVisitEarlyExitCounter counter =
        TileSelectionVisitEarlyExitCounter::None;
    bool markTileCulled = false;
    bool resetScreenSpaceError = false;
    bool queueCulledLoad = false;
    TileLoadPriorityGroup loadGroup = TileLoadPriorityGroup::Preload;
};

struct TileSelectionVisitOutcomePlan {
    bool shouldExit = false;
    TileSelectionVisitEarlyExitReason exitReason =
        TileSelectionVisitEarlyExitReason::None;
    TileSelectionVisitEarlyExitCounter counter =
        TileSelectionVisitEarlyExitCounter::None;
    bool markTileCulled = false;
    bool resetScreenSpaceError = false;
    bool queueLoad = false;
    TileLoadPriorityGroup loadGroup = TileLoadPriorityGroup::Preload;
    bool returnCulledTraversalDetails = false;
    bool countCulledVisited = false;
};

struct TileSelectionVisitPreparation {
    static TileSelectionVisitPreparationResult prepare(
        const TilesetTile& tile,
        const std::vector<FrameState::SelectorView>& views,
        const std::vector<double>& fogDensities,
        const TileSelectionVisibilityContext& visibilityContext,
        const TileSelectionVisitPreparationOptions& options);

    static TileSelectionVisitEarlyExitPlan earlyExitPlan(
        const TileSelectionVisitPreparationResult& preparation);

    static TileSelectionVisitOutcomePlan outcomePlan(
        const TileSelectionVisitPreparationResult& preparation);
};

} // namespace earth_engine
