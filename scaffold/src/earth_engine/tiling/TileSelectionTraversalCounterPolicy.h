#pragma once

#include "TileSelectionPostTraversalPolicy.h"
#include "TileSelectionRefineFlowPolicy.h"
#include "TileSelectionVisitPreparation.h"

namespace earth_engine {

struct TileSelectionTraversalCounterPlan {
    int visited = 0;
    int frustumCulled = 0;
    int fogCulled = 0;
    int culledVisited = 0;
    int occluded = 0;
    int waitingForOcclusion = 0;
    int kicked = 0;
};

struct TileSelectionTraversalCounterPolicy {
    static TileSelectionTraversalCounterPlan planVisitAccepted();
    static TileSelectionTraversalCounterPlan planOutcome(
        const TileSelectionVisitOutcomePlan& outcome);
    static TileSelectionTraversalCounterPlan planRefineFlow(
        const TileSelectionRefineFlowResult& refineFlow);
    static TileSelectionTraversalCounterPlan planPostTraversalCommit(
        const TileSelectionPostTraversalCommitPlan& commitPlan);
};

} // namespace earth_engine
