#pragma once

#include "TilePlan.h"
#include "TileSelectionFrameState.h"
#include <cstddef>

namespace earth_engine {

struct TileSelectionSummaryTileInput {
    TileKey key;
    TileSelectionFrameState selection;
    bool renderable = false;
};

struct TileSelectionSummaryTilePlan {
    bool visited = false;
    TileSelectionRecord record;
    int selectionRenderedCount = 0;
    int selectionRefinedCount = 0;
    int selectionKickedCount = 0;
    int selectionAncestorMeetsSseCount = 0;
    int cameraInsideNodeCount = 0;
    int inFrustumNodeCount = 0;
    int notYetRenderableCount = 0;
};

struct TileSelectionSummaryFrameInput {
    std::size_t visibleTileCount = 0;
    int selectionRefinedCount = 0;
    int selectedTilesCulled = 0;
    int selectedFogCulled = 0;
    int selectedTilesOccluded = 0;
    int selectedTilesWaitingForOcclusionResults = 0;
    int selectedCulledTilesVisited = 0;
};

struct TileSelectionSummaryFramePlan {
    int renderingNodeCount = 0;
    int walkthroughNodeCount = 0;
    int notRenderingNodeCount = 0;
    int selectionOccludedCount = 0;
    int selectionWaitingForOcclusionResultsCount = 0;
    int culledTilesVisitedCount = 0;
    int mercatorTileCount = 0;
};

struct TileSelectionSummaryPolicy {
    static TileSelectionSummaryTilePlan planTile(
        const TileSelectionSummaryTileInput& input);
    static TileSelectionSummaryFramePlan planFrame(
        const TileSelectionSummaryFrameInput& input);
};

} // namespace earth_engine
