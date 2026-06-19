#include "TilesetContentLifecycleFacade.h"

#include "TileContentAccess.h"
#include "TileContentCacheManager.h"
#include "TileContentLifecycleManager.h"
#include "TileMeshPreparationManager.h"
#include "Tileset.h"

namespace earth_engine {

namespace {

constexpr int kSmoothedMainThreadUploadLimit = 1;

} // namespace

TileLoadRequestOutcome TilesetContentLifecycleFacade::requestMissingTiles(
    Tileset& tileset,
    const std::vector<TileLoadRequest>& loadRequests,
    FrameResourceBudget* budget) {
    return tileset.contentLifecycle_.requestMissingTiles(
        loadRequests,
        tileset.terrainProvider_.get(),
        tileset.contentProvider_.get(),
        tileset.device_,
        tileset.tileRegistry_.tiles(),
        tileset.frameNumber_,
        tileset.options_.maximumSimultaneousTileLoads,
        tileset.options_.mainThreadLoadingTimeLimit,
        tileset.currentFrameTimeSeconds_,
        static_cast<uint32_t>(kSmoothedMainThreadUploadLimit),
        budget,
        [&tileset](TilesetTile& tile, double priority) {
            return tileset.meshPreparation_.prepareUpsampleSourceTile(
                tile,
                priority);
        },
        [&tileset](const TileKey& key) {
            return tileset.contentAccess_.ensureTile(key);
        });
}

bool TilesetContentLifecycleFacade::processPendingUploads(
    Tileset& tileset,
    bool interactionActive,
    bool resourceSmoothingActive,
    FrameResourceBudget* budget) {
    return tileset.contentLifecycle_.processPendingUploads(
        tileset.terrainProvider_.get(),
        tileset.contentProvider_.get(),
        tileset.device_,
        tileset.tileRegistry_.tiles(),
        tileset.frameNumber_,
        tileset.options_.maximumSimultaneousTileLoads,
        tileset.options_.mainThreadLoadingTimeLimit,
        tileset.currentFrameTimeSeconds_,
        static_cast<uint32_t>(kSmoothedMainThreadUploadLimit),
        interactionActive,
        resourceSmoothingActive,
        budget,
        [&tileset](const TileKey& key) {
            return tileset.contentAccess_.ensureTile(key);
        },
        [&tileset](TilesetTile& tile) {
            tileset.contentAccess_.ensureTileChildren(tile);
        },
        [&tileset](TilesetTile& tile) {
            tileset.meshPreparation_.ensureTileMesh(tile);
        },
        [&tileset]() {
            markTileResourcesDirty(tileset);
        });
}

void TilesetContentLifecycleFacade::markTileResourcesDirty(Tileset& tileset) {
    ++tileset.resourceRevision_;
    tileset.contentCache_.markResourcesDirty();
    tileset.selectionReuseState_.invalidate();
}

} // namespace earth_engine
