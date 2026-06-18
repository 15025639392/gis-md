#pragma once

#include "TileRefine.h"
#include "TileSelectionRefineFlowPolicy.h"

namespace earth_engine {

enum class TileSelectionPreTraversalFinishReason {
    None,
    CannotRefine,
    DoesNotRefine
};

struct TileSelectionPreTraversalInput {
    bool canRefine = false;
    TileRefine refineMode = TileRefine::Replace;
    TileSelectionRefineFlowResult refineFlow;
};

struct TileSelectionPreTraversalPlan {
    bool queueUrgentLoad = false;
    bool finishAsSingleTile = false;
    TileSelectionPreTraversalFinishReason finishReason =
        TileSelectionPreTraversalFinishReason::None;
    bool singleTileShouldQueueLoad = false;
    bool visitChildren = false;
    bool addAdditiveParentToPlan = false;
    bool additiveParentShouldQueueLoad = false;
    bool queuedForLoadAfterPreTraversal = false;
    bool ancestorMeetsSseAfterPreTraversal = false;
};

struct TileSelectionPreTraversalPolicy {
    static TileSelectionPreTraversalPlan plan(
        const TileSelectionPreTraversalInput& input);
};

} // namespace earth_engine
