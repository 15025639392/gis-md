#include "ScenePrimaryTilesetTakeoverPolicy.h"

#include "../tiling/TilePlan.h"
#include "../tiling/Tileset.h"
#include "../tiling/TilesetTile.h"

#include <algorithm>

namespace earth_engine {
namespace {

constexpr int kMaximumRenderLevelLag = 2;
constexpr int kRequiredConsecutiveReadyFrames = 3;

bool isStableSelectedEntry(const TileRenderEntry& entry) {
    if (!entry.selectedThisFrame || !entry.renderTile) {
        return false;
    }
    if (entry.renderTile->content.renderContent
            .drawsTransientFallbackSurface()) {
        return false;
    }
    return entry.selectedKey.z - entry.renderKey.z <=
           kMaximumRenderLevelLag;
}

bool isAncestorOrSame(const TileKey& ancestor, const TileKey& descendant) {
    if (ancestor.schemeId != descendant.schemeId ||
        ancestor.z > descendant.z) {
        return false;
    }
    const int levelDelta = descendant.z - ancestor.z;
    return (descendant.x >> levelDelta) == ancestor.x &&
           (descendant.y >> levelDelta) == ancestor.y;
}

bool entriesOverlap(
    const TileRenderEntry& first,
    const TileRenderEntry& second) {
    return isAncestorOrSame(first.selectedKey, second.selectedKey) ||
           isAncestorOrSame(second.selectedKey, first.selectedKey);
}

bool keepsCurrentCoverageLevel(
    const TileRenderEntry& pendingEntry,
    const TilePlan& currentPlan) {
    int currentRenderLevel = -1;
    for (const TileRenderEntry& currentEntry : currentPlan.renderEntries) {
        if (!currentEntry.selectedThisFrame ||
            !entriesOverlap(pendingEntry, currentEntry)) {
            continue;
        }
        currentRenderLevel =
            std::max(currentRenderLevel, currentEntry.renderKey.z);
    }
    return currentRenderLevel >= 0 &&
           pendingEntry.renderKey.z + kMaximumRenderLevelLag >=
               currentRenderLevel;
}

int stableSelectedEntryCount(const TilePlan& plan) {
    return static_cast<int>(std::count_if(
        plan.renderEntries.begin(),
        plan.renderEntries.end(),
        isStableSelectedEntry));
}

int selectedEntryCount(const TilePlan& plan) {
    return static_cast<int>(std::count_if(
        plan.renderEntries.begin(),
        plan.renderEntries.end(),
        [](const TileRenderEntry& entry) {
            return entry.selectedThisFrame;
        }));
}

} // namespace

bool ScenePrimaryTilesetTakeoverPolicy::isCandidateReady(
    const Tileset& currentPrimary,
    const Tileset& pendingPrimary) {
    if (pendingPrimary.shouldHoldPresentationFrame()) {
        return false;
    }

    const TilePlan& currentPlan = currentPrimary.tilePlan();
    const TilePlan& pendingPlan = pendingPrimary.tilePlan();
    const int pendingSelectedEntries = selectedEntryCount(pendingPlan);
    if (pendingSelectedEntries == 0 ||
        stableSelectedEntryCount(pendingPlan) != pendingSelectedEntries) {
        return false;
    }
    for (const TileRenderEntry& pendingEntry : pendingPlan.renderEntries) {
        if (pendingEntry.selectedThisFrame &&
            !keepsCurrentCoverageLevel(pendingEntry, currentPlan)) {
            return false;
        }
    }

    const int currentSelectedEntries = selectedEntryCount(currentPlan);
    const int minimumTakeoverEntries =
        std::max(1, std::min(currentSelectedEntries, 32));
    return pendingSelectedEntries >= minimumTakeoverEntries;
}

bool ScenePrimaryTilesetTakeoverPolicy::shouldCommit(
    ScenePrimaryTilesetTakeoverState& state,
    const Tileset& currentPrimary,
    const Tileset& pendingPrimary) {
    if (!isCandidateReady(currentPrimary, pendingPrimary)) {
        state.consecutiveReadyFrames = 0;
        return false;
    }
    ++state.consecutiveReadyFrames;
    return state.consecutiveReadyFrames >=
           kRequiredConsecutiveReadyFrames;
}

} // namespace earth_engine
