#pragma once

#include "TileKey.h"
#include <string>
#include <vector>

namespace earth_engine {

/// TilePlan is the shared, frame-derived candidate set for one tile scheme.
/// It does not decide provider requests or final rendering for a layer.
struct TileTransition {
    TileKey key;
    float opacity = 1.0f;
    int fadingNodeCount = 0;
};

enum class TileSelectionState {
    NotVisited,
    Culled,
    Rendered,
    Refined,
    RenderedAndKicked,
    RefinedAndKicked
};

constexpr bool selectionWasKicked(TileSelectionState state) {
    return state == TileSelectionState::RenderedAndKicked ||
           state == TileSelectionState::RefinedAndKicked;
}

constexpr TileSelectionState originalSelectionState(TileSelectionState state) {
    switch (state) {
        case TileSelectionState::RenderedAndKicked:
            return TileSelectionState::Rendered;
        case TileSelectionState::RefinedAndKicked:
            return TileSelectionState::Refined;
        default:
            return state;
    }
}

constexpr void kickSelectionState(TileSelectionState& state) {
    switch (state) {
        case TileSelectionState::Rendered:
            state = TileSelectionState::RenderedAndKicked;
            break;
        case TileSelectionState::Refined:
            state = TileSelectionState::RefinedAndKicked;
            break;
        default:
            break;
    }
}

struct TileSelectionRecord {
    TileKey key;
    TileSelectionState state = TileSelectionState::NotVisited;
    TileSelectionState previousState = TileSelectionState::NotVisited;
    double screenSpaceError = 0.0;
    bool cameraInside = false;
    bool inFrustum = false;
    bool ancestorMeetsSse = false;
};

struct TilePlan {
    uint64_t frameId = 0;
    int zoom = 0;
    int minVisibleZoom = 0;
    int maxVisibleZoom = 0;
    bool equalZoomApplied = false;
    double lodSizePixels = 0.0;
    double minLodSizePixels = 0.0;
    double maxLodSizePixels = 0.0;
    std::vector<TileKey> visibleTiles;
    std::vector<TileTransition> tilesFadingOut;
    std::vector<TileTransition> tileTransitions;
    std::vector<TileSelectionRecord> selectionRecords;
    int renderingNodeCount = 0;
    int walkthroughNodeCount = 0;
    int notRenderingNodeCount = 0;
    int selectionRenderedCount = 0;
    int selectionRefinedCount = 0;
    int selectionKickedCount = 0;
    int selectionOccludedCount = 0;
    int selectionWaitingForOcclusionResultsCount = 0;
    int culledTilesVisitedCount = 0;
    int selectionAncestorMeetsSseCount = 0;
    int cameraInsideNodeCount = 0;
    int inFrustumNodeCount = 0;
    int horizonTangentPreservedCount = 0;
    int equalZoomSecondPassNodeCount = 0;
    int fadingNodeCount = 0;
    int neighborLinkCount = 0;
    int neighborBalancedTileCount = 0;
    int mercatorTileCount = 0;
    int northPolarTileCount = 0;
    int southPolarTileCount = 0;
};

class TilePlanBuilder {
public:
    static TileKey parentKey(const TileKey& key);
};

} // namespace earth_engine
