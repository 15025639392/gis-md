#include "TileLoadDiagnostics.h"

#include "DirectRasterMapping.h"
#include "TileLoadLifecycle.h"
#include "TileLoadQueue.h"
#include "TileUnloadQueue.h"
#include "TilesetTile.h"

namespace earth_engine {

int TilesetLoadDiagnostics::loadQueueTotal() const {
    return loadQueuePreloadRequests +
           loadQueueNormalRequests +
           loadQueueUrgentRequests;
}

int TilesetLoadDiagnostics::pendingTerrainTotal() const {
    return pendingTerrainRequests +
           pendingGltfTerrainUploads +
           pendingGltfTerrainTerminalResults;
}

int TilesetLoadDiagnostics::pendingContentTotal() const {
    return pendingContentRequests +
           pendingContentUploads +
           pendingContentTerminalResults;
}

TilesetLoadDiagnostics TileLoadDiagnosticsCollector::collect(
    const TileLoadQueue& loadQueue,
    const TileLoadLifecycle& loadLifecycle,
    const FrameResourceBudget& resourceBudget,
    const TileUnloadQueue& unloadQueue,
    const std::unordered_map<std::string, std::unique_ptr<TilesetTile>>&
        tiles) {
    TilesetLoadDiagnostics diag;
    diag.resourceBudget = resourceBudget.snapshot();

    for (const TileLoadRequest& request : loadQueue) {
        switch (request.group) {
            case TileLoadPriorityGroup::Preload:
                ++diag.loadQueuePreloadRequests;
                break;
            case TileLoadPriorityGroup::Normal:
                ++diag.loadQueueNormalRequests;
                break;
            case TileLoadPriorityGroup::Urgent:
                ++diag.loadQueueUrgentRequests;
                break;
        }
    }

    const TileLoadLifecycleCounts lifecycleCounts = loadLifecycle.counts();
    const PendingRequestCounts requestCounts = lifecycleCounts.requests;
    diag.pendingTerrainRequests =
        static_cast<int>(requestCounts.terrainRequests);
    diag.pendingGltfTerrainUploads =
        static_cast<int>(lifecycleCounts.gltfTerrainUploads);
    diag.pendingGltfTerrainTerminalResults =
        static_cast<int>(lifecycleCounts.gltfTerrainTerminalResults);
    diag.pendingContentRequests =
        static_cast<int>(requestCounts.contentRequests);
    diag.pendingContentUploads =
        static_cast<int>(lifecycleCounts.contentUploads);
    diag.pendingContentTerminalResults =
        static_cast<int>(lifecycleCounts.contentTerminalResults);

    diag.unloadQueueTiles = static_cast<int>(unloadQueue.size());

    for (const auto& [cacheKey, tile] : tiles) {
        if (!tile) continue;

        switch (tile->content.loadState) {
            case TileLoadState::Unloading:
                ++diag.loadUnloadingTiles;
                break;
            case TileLoadState::FailedTemporarily:
                ++diag.loadFailedTemporarilyTiles;
                break;
            case TileLoadState::Unloaded:
                ++diag.loadUnloadedTiles;
                break;
            case TileLoadState::ContentLoading:
                ++diag.loadContentLoadingTiles;
                break;
            case TileLoadState::ContentLoaded:
                ++diag.loadContentLoadedTiles;
                break;
            case TileLoadState::Done:
                ++diag.loadDoneTiles;
                break;
            case TileLoadState::Failed:
                ++diag.loadFailedTiles;
                break;
        }

        switch (tile->content.contentKind) {
            case TileContentKind::Unknown:
                ++diag.contentUnknownTiles;
                break;
            case TileContentKind::Empty:
                ++diag.contentEmptyTiles;
                break;
            case TileContentKind::External:
                ++diag.contentExternalTiles;
                break;
            case TileContentKind::Render:
                ++diag.contentRenderTiles;
                break;
        }

        diag.missingRasterOverlayProjections +=
            tile->rasterOverlayState.missingProjectionCount();
        (void)cacheKey;
    }

    return diag;
}

} // namespace earth_engine
