#pragma once

#include "TileKey.h"
#include <array>
#include <string>
#include <vector>

namespace earth_engine {

/// TilePlan is the frame-derived selection result for one tile scheme. The
/// selector owns visible/fading tile decisions and resolves the render entries
/// that the renderer consumes without reselecting LOD.
struct TileTransition {
    TileKey key;
    float opacity = 1.0f;
    int fadingNodeCount = 0;
};

enum class TileRenderEntryReason {
    Direct,
    AncestorFallback,
    SynchronousPrep,
    FadingOut
};

enum class TileRenderEntryPass {
    Selected,
    Fading
};

constexpr const char* tileRenderEntryReasonLabel(
    TileRenderEntryReason reason) {
    switch (reason) {
        case TileRenderEntryReason::Direct:
            return "direct";
        case TileRenderEntryReason::AncestorFallback:
            return "ancestor-fallback";
        case TileRenderEntryReason::SynchronousPrep:
            return "sync-prep";
        case TileRenderEntryReason::FadingOut:
            return "fading-out";
    }
    return "unknown";
}

/// A resolved entry in the frame render result. `selectedKey` is the tile
/// chosen by traversal; `renderKey` is the tile whose render content should be
/// submitted. They differ only for explicit, clipped ancestor fallback.
struct TileRenderEntry {
    TileKey selectedKey;
    TileKey renderKey;
    TileRenderEntryReason reason = TileRenderEntryReason::Direct;
    float opacity = 1.0f;
    bool selectedThisFrame = true;
    bool usesAncestorFallback = false;
    bool allowSynchronousMeshPrep = true;
    bool surfaceClipEnabled = false;
    std::array<float, 4> surfaceClipUv{0.0f, 0.0f, 1.0f, 1.0f};

    bool isAncestorFallback() const {
        return reason == TileRenderEntryReason::AncestorFallback &&
               usesAncestorFallback;
    }

    bool isFadingOut() const {
        return reason == TileRenderEntryReason::FadingOut &&
               !selectedThisFrame;
    }

    bool hasSurfaceClip() const { return surfaceClipEnabled; }

    TileRenderEntryPass renderPass() const {
        return selectedThisFrame ? TileRenderEntryPass::Selected
                                 : TileRenderEntryPass::Fading;
    }
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
    std::vector<TileRenderEntry> renderEntries;
    std::vector<std::string> frameCredits;
    int frameMappedRasterTileCount = 0;
    int frameMappedRasterTileLoadingCount = 0;
    int frameProgressTotalCount = 0;
    int frameProgressLoadingCount = 0;
    double frameLoadProgressPercentage = 100.0;
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
    int renderEntryAncestorFallbackCount = 0;
    int renderEntrySynchronousPrepCount = 0;
    int renderEntryDeferredPrepCount = 0;
    int renderEntryPlannedCommandCount = 0;
    int renderEntrySelectedPlannedCommandCount = 0;
    int renderEntryFadingPlannedCommandCount = 0;
    int renderEntryCommandDrawCount = 0;
    int renderEntrySelectedCommandDrawCount = 0;
    int renderEntryFadingCommandDrawCount = 0;
    int renderEntryCommandMissedDrawCount = 0;
    int renderEntrySelectedCommandMissedDrawCount = 0;
    int renderEntryFadingCommandMissedDrawCount = 0;
    int renderEntryCommandMissingSelectedCount = 0;
    int renderEntryCommandMissingRenderCount = 0;
    int renderEntryCommandDeferredCount = 0;
    int renderEntrySelectedCommandDeferredCount = 0;
    int renderEntryFadingCommandDeferredCount = 0;
};

class TilePlanBuilder {
public:
    static TileKey parentKey(const TileKey& key);
};

} // namespace earth_engine
