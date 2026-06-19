#include "TilesetContentLifecycleFacade.h"

#include "TileContentRuntime.h"
#include "Tileset.h"

namespace earth_engine {

namespace {

constexpr int kSmoothedMainThreadUploadLimit = 1;

} // namespace

TileLoadRequestOutcome TilesetContentLifecycleFacade::requestMissingTiles(
    Tileset& tileset,
    const std::vector<TileLoadRequest>& loadRequests,
    FrameResourceBudget* budget) {
    return tileset.contentRuntime_.requestMissingTiles(
        loadRequests,
        TileContentRuntimeFrame{
            tileset.terrainProvider_.get(),
            tileset.contentProvider_.get(),
            tileset.device_,
            tileset.tileRegistry_.tiles(),
            tileset.frameNumber_,
            tileset.options_.maximumSimultaneousTileLoads,
            tileset.options_.mainThreadLoadingTimeLimit,
            tileset.currentFrameTimeSeconds_,
            static_cast<uint32_t>(kSmoothedMainThreadUploadLimit)},
        budget);
}

bool TilesetContentLifecycleFacade::processPendingUploads(
    Tileset& tileset,
    bool interactionActive,
    bool resourceSmoothingActive,
    FrameResourceBudget* budget) {
    return tileset.contentRuntime_.processPendingUploads(
        TileContentRuntimeFrame{
            tileset.terrainProvider_.get(),
            tileset.contentProvider_.get(),
            tileset.device_,
            tileset.tileRegistry_.tiles(),
            tileset.frameNumber_,
            tileset.options_.maximumSimultaneousTileLoads,
            tileset.options_.mainThreadLoadingTimeLimit,
            tileset.currentFrameTimeSeconds_,
            static_cast<uint32_t>(kSmoothedMainThreadUploadLimit)},
        interactionActive,
        resourceSmoothingActive,
        budget);
}

void TilesetContentLifecycleFacade::markTileResourcesDirty(Tileset& tileset) {
    tileset.contentRuntime_.markResourcesDirty();
}

} // namespace earth_engine
