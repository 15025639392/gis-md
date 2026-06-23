#include "TileRenderPlanFrameRefresher.h"

#include "TileCacheKey.h"
#include "TileContentAccess.h"
#include "RasterMappedToTilesetTile.h"
#include "TileRenderPlanFinalizer.h"
#include "TilesetTile.h"
#include "../layers/ActivatedRasterOverlay.h"
#include "../providers/ImageryProvider.h"
#include "../providers/RasterOverlayTile.h"
#include "../providers/RasterOverlayTileProvider.h"

#include <algorithm>
#include <vector>

namespace earth_engine {

namespace {

constexpr int kActiveInteractionRenderPrepBudget = 0;
constexpr int kRecoveryRenderPrepBudget = 1;

void appendUniqueCredit(std::vector<std::string>& frameCredits,
                        const std::string& credit) {
    if (credit.empty()) {
        return;
    }
    if (std::find(frameCredits.begin(), frameCredits.end(), credit) ==
        frameCredits.end()) {
        frameCredits.push_back(credit);
    }
}

void collectReadyRasterTileCredits(const TilesetTile& tile,
                                   std::vector<std::string>& frameCredits) {
    tile.rasterOverlayState.forEachMapping(
        [&frameCredits](const RasterMappedToTilesetTile* mapping) {
            if (!mapping) {
                return;
            }

            const RasterOverlayTile* readyTile = mapping->getReadyTile();
            if (!readyTile) {
                return;
            }

            for (const std::string& credit : readyTile->credits()) {
                appendUniqueCredit(frameCredits, credit);
            }
        });
}

void collectRasterOverlayProviderCredits(
    const std::vector<ActivatedRasterOverlay*>& rasterOverlays,
    std::vector<std::string>& frameCredits) {
    for (const ActivatedRasterOverlay* overlay : rasterOverlays) {
        if (!overlay) {
            continue;
        }

        const RasterOverlayTileProvider* tileProvider =
            overlay->getTileProvider();
        if (!tileProvider) {
            continue;
        }

        appendUniqueCredit(
            frameCredits,
            tileProvider->getImageryProvider().attribution());
    }
}

void refreshFrameCredits(TilePlan& tilePlan,
                         const std::vector<ActivatedRasterOverlay*>&
                             rasterOverlays,
                         TileContentAccess& contentAccess) {
    tilePlan.frameCredits.clear();
    collectRasterOverlayProviderCredits(rasterOverlays, tilePlan.frameCredits);

    for (const TileRenderEntry& entry : tilePlan.renderEntries) {
        if (TilesetTile* selectedTile =
                contentAccess.ensureTile(entry.selectedKey)) {
            collectReadyRasterTileCredits(*selectedTile, tilePlan.frameCredits);
        }

        if (entry.renderKey != entry.selectedKey) {
            if (TilesetTile* renderTile =
                    contentAccess.ensureTile(entry.renderKey)) {
                collectReadyRasterTileCredits(
                    *renderTile,
                    tilePlan.frameCredits);
            }
        }
    }
}

bool containsTileKey(const std::vector<TileKey>& keys, const TileKey& key) {
    return std::find(keys.begin(), keys.end(), key) != keys.end();
}

void collectMappedRasterProgress(const TilesetTile& tile,
                                 TilePlan& tilePlan) {
    tile.rasterOverlayState.forEachMapping(
        [&tilePlan](const RasterMappedToTilesetTile* mapping) {
            if (!mapping) {
                return;
            }

            ++tilePlan.frameMappedRasterTileCount;
            if (mapping->getState() !=
                RasterMappedToTilesetTile::State::Attached) {
                ++tilePlan.frameMappedRasterTileLoadingCount;
            }
        });
}

int baseProgressTotalCount(const TilePlan& tilePlan) {
    const int selectedTraversalCount =
        tilePlan.renderingNodeCount +
        tilePlan.walkthroughNodeCount +
        tilePlan.notRenderingNodeCount;
    if (selectedTraversalCount > 0) {
        return selectedTraversalCount;
    }

    return static_cast<int>(
        tilePlan.visibleTiles.size() + tilePlan.tilesFadingOut.size());
}

void refreshFrameProgress(TilePlan& tilePlan,
                          TileContentAccess& contentAccess) {
    tilePlan.frameMappedRasterTileCount = 0;
    tilePlan.frameMappedRasterTileLoadingCount = 0;

    std::vector<TileKey> visitedTiles;
    visitedTiles.reserve(tilePlan.renderEntries.size() * 2);
    for (const TileRenderEntry& entry : tilePlan.renderEntries) {
        if (!containsTileKey(visitedTiles, entry.selectedKey)) {
            visitedTiles.push_back(entry.selectedKey);
            if (TilesetTile* selectedTile =
                    contentAccess.ensureTile(entry.selectedKey)) {
                collectMappedRasterProgress(*selectedTile, tilePlan);
            }
        }

        if (entry.renderKey != entry.selectedKey &&
            !containsTileKey(visitedTiles, entry.renderKey)) {
            visitedTiles.push_back(entry.renderKey);
            if (TilesetTile* renderTile =
                    contentAccess.ensureTile(entry.renderKey)) {
                collectMappedRasterProgress(*renderTile, tilePlan);
            }
        }
    }

    tilePlan.frameProgressTotalCount =
        baseProgressTotalCount(tilePlan) + tilePlan.frameMappedRasterTileCount;
    tilePlan.frameProgressLoadingCount =
        tilePlan.frameMappedRasterTileLoadingCount;
    tilePlan.frameLoadProgressPercentage =
        tilePlan.frameProgressLoadingCount == 0 ||
                tilePlan.frameProgressTotalCount <= 0
            ? 100.0
            : 100.0 *
                  static_cast<double>(
                      tilePlan.frameProgressTotalCount -
                      tilePlan.frameProgressLoadingCount) /
                  static_cast<double>(tilePlan.frameProgressTotalCount);
}

} // namespace

void TileRenderPlanFrameRefresher::refresh(
    TilePlan& tilePlan,
    TileContentAccess& contentAccess,
    const std::vector<ActivatedRasterOverlay*>& rasterOverlays,
    const TileRenderPlanFrameRefreshOptions& options) {
    TileRenderPlanFinalizer::refreshRenderEntries(
        tilePlan,
        TileRenderPlanFinalizeOptions{
            options.enableLodTransitionPeriod,
            options.interactionActive,
            kActiveInteractionRenderPrepBudget,
            kRecoveryRenderPrepBudget},
        [&contentAccess](const TileKey& key) {
            return contentAccess.ensureTile(key);
        },
        [](const TileKey& key) {
            return TileCacheKey::forTile(key);
        },
        [](const TilesetTile& tile) {
            return tile.hasSurfaceDrawable() ||
                   tile.content.renderContent.isGltfRenderReady();
        });
    refreshFrameCredits(tilePlan, rasterOverlays, contentAccess);
    refreshFrameProgress(tilePlan, contentAccess);
}

} // namespace earth_engine
