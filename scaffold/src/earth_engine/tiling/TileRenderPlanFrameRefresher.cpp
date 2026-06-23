#include "TileRenderPlanFrameRefresher.h"

#include "TileCacheKey.h"
#include "TileContentAccess.h"
#include "RasterMappedToTilesetTile.h"
#include "TileRenderPlanFinalizer.h"
#include "TilesetTile.h"
#include "../providers/RasterOverlayTile.h"

#include <algorithm>

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

void refreshFrameCredits(TilePlan& tilePlan,
                         TileContentAccess& contentAccess) {
    tilePlan.frameCredits.clear();

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

} // namespace

void TileRenderPlanFrameRefresher::refresh(
    TilePlan& tilePlan,
    TileContentAccess& contentAccess,
    const std::vector<ActivatedRasterOverlay*>& rasterOverlays,
    const TileRenderPlanFrameRefreshOptions& options) {
    (void)rasterOverlays;
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
    refreshFrameCredits(tilePlan, contentAccess);
}

} // namespace earth_engine
