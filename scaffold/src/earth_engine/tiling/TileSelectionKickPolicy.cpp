#include "TileSelectionKickPolicy.h"

namespace earth_engine {

bool TileSelectionKickPolicy::shouldKickDescendants(
    const TileTraversalDetails& traversalDetails,
    bool renderable,
    bool /*unconditionallyRefine*/,
    uint32_t loadingDescendantLimit,
    bool enableLodTransitionPeriod,
    bool kickDescendantsWhileFadingIn,
    TileSelectionState previousSelectionState,
    bool hasLodTransitionRenderContent,
    float lodTransitionFadePercentage) {
    const bool kickDueToNonReadyDescendant =
        !traversalDetails.allAreRenderable &&
        !traversalDetails.anyWereRenderedLastFrame;
    const bool kickDueToTileFadingIn =
        enableLodTransitionPeriod &&
        kickDescendantsWhileFadingIn &&
        previousSelectionState == TileSelectionState::Rendered &&
        hasLodTransitionRenderContent &&
        lodTransitionFadePercentage < 1.0f;

    return (kickDueToNonReadyDescendant ||
            kickDueToTileFadingIn) &&
           (traversalDetails.notYetRenderableCount > loadingDescendantLimit ||
            renderable);
}

bool TileSelectionKickPolicy::shouldRestoreChildLoadQueueAndLoadParent(
    const TileTraversalDetails& traversalDetails,
    bool wasReallyRenderedLastFrame,
    uint32_t loadingDescendantLimit,
    bool isExternalContent,
    bool unconditionallyRefine) {
    return !wasReallyRenderedLastFrame &&
           traversalDetails.notYetRenderableCount > loadingDescendantLimit &&
           !isExternalContent &&
           !unconditionallyRefine;
}

TileSelectionKickPlan TileSelectionKickPolicy::planAfterKick(
    const TileTraversalDetails& traversalDetails,
    bool wasReallyRenderedLastFrame,
    uint32_t loadingDescendantLimit,
    bool isExternalContent,
    bool unconditionallyRefine,
    TileRefine refineMode,
    bool queuedForLoad,
    bool preloadAncestors) {
    TileSelectionKickPlan plan;
    plan.restoreChildLoadQueueAndLoadParent =
        shouldRestoreChildLoadQueueAndLoadParent(
            traversalDetails,
            wasReallyRenderedLastFrame,
            loadingDescendantLimit,
            isExternalContent,
            unconditionallyRefine);

    const bool willQueueParent =
        queuedForLoad || plan.restoreChildLoadQueueAndLoadParent;
    // cesium TilesetSelection.cpp:730-732: the kicked tile replaces its
    // descendants in the render list for ANY non-additive refine — even a
    // not-yet-renderable one (it draws nothing this frame). Gating on
    // renderable diverged: S2 对拍 restore 帧 cesium render=[0-0-0] 而
    // gis-md render=[]。
    plan.addReplacementToPlan = refineMode != TileRefine::Add;
    plan.preloadParent = preloadAncestors && !willQueueParent;
    return plan;
}

} // namespace earth_engine
