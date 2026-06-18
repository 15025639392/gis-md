#pragma once

#include "TilePlan.h"
#include "TileRefine.h"

#include <cstdint>

namespace earth_engine {

struct TileTraversalDetails {
    bool allAreRenderable = true;
    bool anyWereRenderedLastFrame = false;
    uint32_t notYetRenderableCount = 0;
};

struct TileTraversalDetailsPolicy {
    static bool wasRenderedLastFrameForTraversalDetails(
        TileSelectionState previousSelectionState,
        TileRefine refine,
        bool anyDescendantWasRenderedLastFrame);

    static TileTraversalDetails forSingleTile(
        bool renderable,
        bool wasRenderedLastFrame);

    static TileTraversalDetails forCulledTile(
        bool forbidHoles,
        TileRefine refine,
        bool renderable,
        bool wasRenderedLastFrame);

    static void mergeChild(TileTraversalDetails& aggregate,
                           const TileTraversalDetails& child);
};

} // namespace earth_engine
