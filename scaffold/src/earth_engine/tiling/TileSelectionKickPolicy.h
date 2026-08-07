#pragma once

#include "TilePlan.h"
#include "TileRefine.h"
#include "TileTraversalDetails.h"

#include <cstdint>

namespace earth_engine {

struct TileSelectionKickPlan {
    bool restoreChildLoadQueueAndLoadParent = false;
    bool addReplacementToPlan = false;
    bool preloadParent = false;
};

struct TileSelectionKickPolicy {
    static bool shouldKickDescendants(
        const TileTraversalDetails& traversalDetails,
        bool renderable,
        bool unconditionallyRefine,
        uint32_t loadingDescendantLimit);

    static bool shouldRestoreChildLoadQueueAndLoadParent(
        const TileTraversalDetails& traversalDetails,
        bool wasReallyRenderedLastFrame,
        uint32_t loadingDescendantLimit,
        bool isExternalContent,
        bool unconditionallyRefine);

    static TileSelectionKickPlan planAfterKick(
        const TileTraversalDetails& traversalDetails,
        bool wasReallyRenderedLastFrame,
        uint32_t loadingDescendantLimit,
        bool isExternalContent,
        bool unconditionallyRefine,
        TileRefine refineMode,
        bool queuedForLoad,
        bool preloadAncestors);
};

} // namespace earth_engine
