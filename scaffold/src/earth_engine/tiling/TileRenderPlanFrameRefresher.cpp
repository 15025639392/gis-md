#include "TileRenderPlanFrameRefresher.h"

#include "TileCacheKey.h"
#include "TileContentAccess.h"
#include "RasterMappedToTilesetTile.h"
#include "TileRasterOverlayReadinessPolicy.h"
#include "TileRenderPlanFinalizer.h"
#include "TilesetTile.h"
#include "../layers/ActivatedRasterOverlay.h"
#include "../providers/ImageryProvider.h"
#include "../providers/RasterOverlayTile.h"
#include "../providers/RasterOverlayTileProvider.h"

#include <unordered_set>
#include <vector>

namespace earth_engine {

namespace {

constexpr int kActiveInteractionRenderPrepBudget = 0;
constexpr int kRecoveryRenderPrepBudget = 1;

// 去重集合 + 输出 vector 并行维护：vector 保持插入序（展示顺序稳定），
// set 提供 O(1) 查重——原先逐条 std::find 在瓦片×credit 数上是 O(N²)
// 字符串比较（P2-10）。
struct FrameCreditCollector {
    std::vector<std::string>& frameCredits;
    std::unordered_set<std::string> seen;

    void append(const std::string& credit) {
        if (credit.empty()) {
            return;
        }
        if (seen.insert(credit).second) {
            frameCredits.push_back(credit);
        }
    }
};

void collectReadyRasterTileCredits(const TilesetTile& tile,
                                   FrameCreditCollector& credits) {
    tile.rasterOverlayState.forEachMapping(
        [&credits](const RasterMappedToTilesetTile* mapping) {
            if (!mapping) {
                return;
            }

            const RasterOverlayTile* readyTile = mapping->getReadyTile();
            if (!readyTile) {
                return;
            }

            for (const std::string& credit : readyTile->credits()) {
                credits.append(credit);
            }
        });
}

void collectRenderContentCredits(const TilesetTile& tile,
                                 FrameCreditCollector& credits) {
    for (const std::string& credit : tile.content.renderContent.credits()) {
        credits.append(credit);
    }
}

void collectRasterOverlayProviderCredits(
    const std::vector<ActivatedRasterOverlay*>& rasterOverlays,
    FrameCreditCollector& credits) {
    for (const ActivatedRasterOverlay* overlay : rasterOverlays) {
        if (!overlay) {
            continue;
        }

        const RasterOverlayTileProvider* tileProvider =
            overlay->getTileProvider();
        if (!tileProvider) {
            continue;
        }

        credits.append(tileProvider->getImageryProvider().attribution());
    }
}

void refreshFrameCredits(TilePlan& tilePlan,
                         const std::vector<ActivatedRasterOverlay*>&
                             rasterOverlays) {
    tilePlan.frameCredits.clear();
    FrameCreditCollector credits{tilePlan.frameCredits, {}};
    collectRasterOverlayProviderCredits(rasterOverlays, credits);

    for (const TileRenderEntry& entry : tilePlan.renderEntries) {
        if (entry.selectedTile) {
            collectRenderContentCredits(*entry.selectedTile, credits);
            collectReadyRasterTileCredits(*entry.selectedTile, credits);
        }

        if (entry.renderTile &&
            entry.renderTile != entry.selectedTile) {
            collectRenderContentCredits(*entry.renderTile, credits);
            collectReadyRasterTileCredits(*entry.renderTile, credits);
        }
    }
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

void refreshFrameProgress(TilePlan& tilePlan) {
    tilePlan.frameMappedRasterTileCount = 0;
    tilePlan.frameMappedRasterTileLoadingCount = 0;

    std::unordered_set<TilesetTile*> visitedTiles;
    visitedTiles.reserve(tilePlan.renderEntries.size() * 2);
    auto visitTile = [&](TilesetTile* tile) {
        if (!tile || !visitedTiles.insert(tile).second) {
            return;
        }
        collectMappedRasterProgress(*tile, tilePlan);
    };

    for (TilesetTile* tile : tilePlan.tilesToRenderThisFrame) {
        visitTile(tile);
    }
    for (TilesetTile* tile : tilePlan.tilesFadingOutThisFrame) {
        visitTile(tile);
    }

    for (const TileRenderEntry& entry : tilePlan.renderEntries) {
        visitTile(entry.selectedTile);
        visitTile(entry.renderTile);
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
            kRecoveryRenderPrepBudget,
            options.maximumScreenSpaceError},
        rasterOverlays,
        [&contentAccess](const TileKey& key) {
            return contentAccess.ensureTile(key);
        },
        [](const TileKey& key) {
            return TileCacheKey::forTile(key);
        },
        [&rasterOverlays](const TilesetTile& tile) {
            const bool geometryDrawable =
                tile.content.renderContent.hasDrawableResources();
            return geometryDrawable &&
                   TileRasterOverlayReadinessPolicy::
                       terrainSurfaceImageryDrawableReady(
                           tile,
                           rasterOverlays);
        });
    refreshFrameCredits(tilePlan, rasterOverlays);
    refreshFrameProgress(tilePlan);
}

} // namespace earth_engine
