#include "TileTraversalDetails.h"

namespace earth_engine {

bool TileTraversalDetailsPolicy::wasRenderedLastFrameForTraversalDetails(
    TileSelectionState previousSelectionState,
    TileRefine refine,
    bool anyDescendantWasRenderedLastFrame) {
    const TileSelectionState previousState =
        originalSelectionState(previousSelectionState);
    return previousState == TileSelectionState::Rendered ||
           (previousState == TileSelectionState::Refined &&
            (refine == TileRefine::Add ||
             anyDescendantWasRenderedLastFrame));
}

TileTraversalDetails TileTraversalDetailsPolicy::forSingleTile(
    bool renderable,
    bool wasRenderedLastFrame) {
    TileTraversalDetails details;
    details.allAreRenderable = renderable;
    details.anyWereRenderedLastFrame = renderable && wasRenderedLastFrame;
    details.notYetRenderableCount = renderable ? 0 : 1;
    return details;
}

TileTraversalDetails TileTraversalDetailsPolicy::forCulledTile(
    bool forbidHoles,
    TileRefine refine,
    bool renderable,
    bool wasRenderedLastFrame) {
    if (forbidHoles && refine == TileRefine::Replace) {
        return forSingleTile(renderable, wasRenderedLastFrame);
    }
    return TileTraversalDetails{};
}

void TileTraversalDetailsPolicy::mergeChild(
    TileTraversalDetails& aggregate,
    const TileTraversalDetails& child) {
    aggregate.allAreRenderable &= child.allAreRenderable;
    aggregate.anyWereRenderedLastFrame |= child.anyWereRenderedLastFrame;
    aggregate.notYetRenderableCount += child.notYetRenderableCount;
}

} // namespace earth_engine
