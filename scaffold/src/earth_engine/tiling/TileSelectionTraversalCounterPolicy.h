#pragma once

#include "TileSelectionRefineFlowPolicy.h"
#include "TileSelectionVisitPreparation.h"

namespace earth_engine {

// NOTE: the kicked counter is NOT planned here — cesium semantics
// (TilesetSelection.cpp:756) count restored load-queue entries, which
// TileSelectionPostTraversalCommitter accumulates directly in its restore
// branch.
struct TileSelectionTraversalCounterPlan {
    int visited = 0;
    int frustumCulled = 0;
    int fogCulled = 0;
    int culledVisited = 0;
    int occluded = 0;
    int waitingForOcclusion = 0;
};

struct TileSelectionTraversalCounterPolicy {
    static TileSelectionTraversalCounterPlan planVisitAccepted();
    static TileSelectionTraversalCounterPlan planOutcome(
        const TileSelectionVisitOutcomePlan& outcome);
    static TileSelectionTraversalCounterPlan planRefineFlow(
        const TileSelectionRefineFlowResult& refineFlow);
};

} // namespace earth_engine
