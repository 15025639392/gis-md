#include "TileSelectionTraversalCounterPolicy.h"

namespace earth_engine {

TileSelectionTraversalCounterPlan
TileSelectionTraversalCounterPolicy::planVisitAccepted() {
    TileSelectionTraversalCounterPlan plan;
    plan.visited = 1;
    return plan;
}

TileSelectionTraversalCounterPlan
TileSelectionTraversalCounterPolicy::planOutcome(
    const TileSelectionVisitOutcomePlan& outcome) {
    TileSelectionTraversalCounterPlan plan;
    if (outcome.counter == TileSelectionVisitEarlyExitCounter::FrustumCulled) {
        plan.frustumCulled = 1;
    } else if (
        outcome.counter == TileSelectionVisitEarlyExitCounter::FogCulled) {
        plan.fogCulled = 1;
    }
    if (outcome.countCulledVisited) {
        plan.culledVisited = 1;
    }
    return plan;
}

TileSelectionTraversalCounterPlan
TileSelectionTraversalCounterPolicy::planRefineFlow(
    const TileSelectionRefineFlowResult& refineFlow) {
    TileSelectionTraversalCounterPlan plan;
    if (refineFlow.counter == TileSelectionRefineFlowCounter::Occluded) {
        plan.occluded = 1;
    } else if (
        refineFlow.counter ==
        TileSelectionRefineFlowCounter::WaitingForOcclusion) {
        plan.waitingForOcclusion = 1;
    }
    return plan;
}

TileSelectionTraversalCounterPlan
TileSelectionTraversalCounterPolicy::planPostTraversalCommit(
    const TileSelectionPostTraversalCommitPlan& commitPlan) {
    TileSelectionTraversalCounterPlan plan;
    if (commitPlan.trimRenderedDescendants) {
        plan.kicked = 1;
    }
    return plan;
}

} // namespace earth_engine
