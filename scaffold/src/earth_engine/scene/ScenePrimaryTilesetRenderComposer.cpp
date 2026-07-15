#include "ScenePrimaryTilesetRenderComposer.h"

#include "../tiling/TileScheme.h"
#include "../tiling/TileSurfaceClip.h"
#include "../tiling/Tileset.h"
#include "../tiling/TilesetTile.h"

#include <algorithm>
#include <array>
#include <utility>

namespace earth_engine {
namespace {

constexpr int kMaximumRenderLevelLag = 2;

bool isAncestorOrSame(
    const TileKey& ancestor,
    const TileKey& descendant) {
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

bool isStableRealTerrainEntry(const TileRenderEntry& entry) {
    if (!entry.selectedThisFrame || !entry.renderTile) {
        return false;
    }
    const TileRenderContentState& renderContent =
        entry.renderTile->content.renderContent;
    if (renderContent.drawsFill()) {
        return false;
    }
    const SurfaceDrawableSource source =
        renderContent.currentSurfaceSource();
    return source == SurfaceDrawableSource::HeightmapTerrain ||
           source == SurfaceDrawableSource::GltfContent;
}

bool keepsCurrentCoverageLevel(
    const TileRenderEntry& pendingEntry,
    const TilePlan& currentPlan) {
    bool overlapsCurrent = false;
    for (const TileRenderEntry& currentEntry : currentPlan.renderEntries) {
        if (!currentEntry.selectedThisFrame ||
            !entriesOverlap(pendingEntry, currentEntry)) {
            continue;
        }
        overlapsCurrent = true;
        if (pendingEntry.renderKey.z + kMaximumRenderLevelLag <
            currentEntry.renderKey.z) {
            return false;
        }
    }
    return overlapsCurrent;
}

std::array<TileKey, 4> childrenOf(const TileKey& key) {
    const int childZ = key.z + 1;
    const int childX = key.x * 2;
    const int childY = key.y * 2;
    return {
        TileKey{key.schemeId, childZ, childX, childY},
        TileKey{key.schemeId, childZ, childX + 1, childY},
        TileKey{key.schemeId, childZ, childX, childY + 1},
        TileKey{key.schemeId, childZ, childX + 1, childY + 1}};
}

bool appendUncoveredRegions(
    const TileKey& region,
    const std::vector<TileKey>& pendingCoverage,
    std::vector<TileKey>& uncovered) {
    bool hasDescendantCoverage = false;
    for (const TileKey& coverage : pendingCoverage) {
        if (isAncestorOrSame(coverage, region)) {
            return true;
        }
        if (isAncestorOrSame(region, coverage)) {
            hasDescendantCoverage = true;
        }
    }
    if (!hasDescendantCoverage) {
        uncovered.push_back(region);
        return true;
    }
    for (const TileKey& child : childrenOf(region)) {
        if (!appendUncoveredRegions(child, pendingCoverage, uncovered)) {
            return false;
        }
    }
    return true;
}

bool appendCurrentDifference(
    const Tileset& currentPrimary,
    const TileRenderEntry& currentEntry,
    const std::vector<TileKey>& pendingCoverage,
    std::vector<TileRenderEntry>& output) {
    std::vector<TileKey> uncovered;
    if (!appendUncoveredRegions(
            currentEntry.selectedKey,
            pendingCoverage,
            uncovered)) {
        return false;
    }
    for (const TileKey& region : uncovered) {
        if (region == currentEntry.selectedKey) {
            output.push_back(currentEntry);
            continue;
        }
        if (!currentEntry.renderTile) {
            return false;
        }
        const auto clip = TileSurfaceClip::forDescendantBounds(
            *currentEntry.renderTile,
            currentPrimary.tileScheme().tileToRectangle(region));
        if (!clip) {
            return false;
        }
        TileRenderEntry fragment = currentEntry;
        fragment.selectedKey = region;
        fragment.surfaceClipEnabled = true;
        fragment.surfaceClipUv = *clip;
        output.push_back(std::move(fragment));
    }
    return true;
}

} // namespace

ScenePrimaryTilesetRenderComposition
ScenePrimaryTilesetRenderComposer::compose(
    const Tileset& currentPrimary,
    const Tileset& pendingPrimary) {
    ScenePrimaryTilesetRenderComposition result;
    const TilePlan& currentPlan = currentPrimary.tilePlan();
    const TilePlan& pendingPlan = pendingPrimary.tilePlan();

    if (currentPrimary.tileScheme().id() !=
        pendingPrimary.tileScheme().id()) {
        result.currentEntries = currentPlan.renderEntries;
        return result;
    }

    std::vector<TileKey> pendingCoverage;
    for (const TileRenderEntry& entry : pendingPlan.renderEntries) {
        if (!isStableRealTerrainEntry(entry) ||
            !keepsCurrentCoverageLevel(entry, currentPlan)) {
            continue;
        }
        result.pendingEntries.push_back(entry);
        pendingCoverage.push_back(entry.selectedKey);
    }
    if (pendingCoverage.empty()) {
        result.currentEntries = currentPlan.renderEntries;
        return result;
    }

    for (const TileRenderEntry& currentEntry : currentPlan.renderEntries) {
        if (!appendCurrentDifference(
                currentPrimary,
                currentEntry,
                pendingCoverage,
                result.currentEntries)) {
            result = {};
            result.currentEntries = currentPlan.renderEntries;
            return result;
        }
    }
    result.replacedRegionCount =
        static_cast<int>(result.pendingEntries.size());
    return result;
}

} // namespace earth_engine
