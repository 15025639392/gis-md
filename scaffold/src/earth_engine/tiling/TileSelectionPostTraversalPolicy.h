#pragma once

#include "TileRefine.h"
#include "TileSelectionKickPolicy.h"
#include "TileTraversalDetails.h"

#include <cstdint>

namespace earth_engine {

struct TileSelectionPostTraversalOptions {
    uint32_t loadingDescendantLimit = 20;
    bool enableLodTransitionPeriod = true;
    bool kickDescendantsWhileFadingIn = true;
    bool preloadAncestors = true;
};

struct TileSelectionPostTraversalInput {
    TileTraversalDetails traversalDetails;
    bool renderable = false;
    bool unconditionallyRefine = false;
    TileSelectionState previousSelectionState = TileSelectionState::NotVisited;
    bool hasLodTransitionRenderContent = false;
    float lodTransitionFadePercentage = 1.0f;
    bool wasRenderedLastFrame = false;
    bool isExternalContent = false;
    TileRefine refineMode = TileRefine::Replace;
    bool queuedForLoad = false;
};

struct TileSelectionPostTraversalResult {
    bool shouldKick = false;
    bool wasReallyRenderedLastFrame = false;
    TileSelectionKickPlan kickPlan;
    bool preloadRefinedAncestor = false;
};

struct TileSelectionPostTraversalCommitPlan {
    bool kickVisitedDescendants = false;
    bool trimRenderedDescendants = false;
    bool restoreChildLoadQueue = false;
    bool queueParentNormal = false;
    bool addRenderableReplacementToPlan = false;
    bool queueParentPreload = false;
    bool markTileRefined = false;
    bool returnSingleTileDetails = false;
    bool wasReallyRenderedLastFrame = false;
};

struct TileSelectionPostTraversalPolicy {
    static TileSelectionPostTraversalResult evaluate(
        const TileSelectionPostTraversalInput& input,
        const TileSelectionPostTraversalOptions& options);

    static TileSelectionPostTraversalCommitPlan commitPlan(
        const TileSelectionPostTraversalResult& result,
        bool queuedForLoad);
};

} // namespace earth_engine
