#include "TileContentRuntime.h"

#include "TileContentAccess.h"
#include "TileContentLifecycleManager.h"
#include "TileContentResourceInvalidator.h"
#include "TileKey.h"
#include "TileMeshPreparationManager.h"

namespace earth_engine {

TileContentRuntime::TileContentRuntime(
    TileContentLifecycleManager& lifecycle,
    TileContentAccess& contentAccess,
    TileMeshPreparationManager& meshPreparation,
    TileContentResourceInvalidator& resourceInvalidator)
    : lifecycle_(lifecycle),
      contentAccess_(contentAccess),
      meshPreparation_(meshPreparation),
      resourceInvalidator_(resourceInvalidator) {}

TileLoadRequestOutcome TileContentRuntime::requestMissingTiles(
    const std::vector<TileLoadRequest>& loadRequests,
    const TileContentRuntimeRequestFrame& frame,
    FrameResourceBudget* budget) {
    return lifecycle_.requestMissingTiles(
        loadRequests,
        frame.contentProvider,
        frame.device,
        frame.rasterOverlays,
        frame.tiles,
        frame.frameNumber,
        frame.maximumSimultaneousTileLoads,
        frame.mainThreadLoadingTimeLimit,
        frame.currentFrameTimeSeconds,
        frame.smoothedMainThreadUploadLimit,
        budget,
        [this, &frame](TilesetTile& tile, double priority) {
            return meshPreparation_.prepareUpsampleSourceTile(
                tile,
                priority,
                frame.pPrepRenderer);
        },
        [this](const TileKey& key) {
            return contentAccess_.ensureTile(key);
        });
}

bool TileContentRuntime::processPendingUploads(
    const TileContentRuntimeUploadFrame& frame,
    bool interactionActive,
    bool resourceSmoothingActive,
    FrameResourceBudget* budget) {
    return lifecycle_.processPendingUploads(
        frame.contentProvider,
        frame.device,
        frame.pPrepRenderer,
        frame.rasterOverlays,
        frame.gpuUploadQueue,
        frame.frameNumber,
        frame.maximumSimultaneousTileLoads,
        frame.mainThreadLoadingTimeLimit,
        frame.currentFrameTimeSeconds,
        frame.smoothedMainThreadUploadLimit,
        interactionActive,
        resourceSmoothingActive,
        budget,
        [this](const TileKey& key) {
            return contentAccess_.ensureTile(key);
        },
        [this, &frame](TilesetTile& tile) {
            contentAccess_.ensureTileChildren(tile, frame.pPrepRenderer);
        },
        [this]() {
            markResourcesDirty();
        });
}

bool TileContentRuntime::drainGpuUploadQueue(
    const TileContentRuntimeUploadFrame& frame,
    FrameResourceBudget* budget,
    uint32_t maxUploadsPerFrame) {
    return lifecycle_.drainGpuUploadQueue(
        frame.contentProvider,
        frame.device,
        frame.pPrepRenderer,
        frame.rasterOverlays,
        frame.gpuUploadQueue,
        frame.frameNumber,
        frame.maximumSimultaneousTileLoads,
        frame.mainThreadLoadingTimeLimit,
        frame.currentFrameTimeSeconds,
        frame.smoothedMainThreadUploadLimit,
        budget,
        maxUploadsPerFrame,
        [this](const TileKey& key) {
            return contentAccess_.ensureTile(key);
        },
        [this]() {
            markResourcesDirty();
        });
}

void TileContentRuntime::markResourcesDirty() {
    resourceInvalidator_.markResourcesDirty();
}

} // namespace earth_engine
