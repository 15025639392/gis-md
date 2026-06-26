#include "TileMeshPreparationManager.h"

#include "TileContentLifecycleManager.h"
#include "TileContentResourceInvalidator.h"
#include "TileLoadQueue.h"
#include "TileUpsampleSourcePreparer.h"
#include "TilesetTile.h"

namespace earth_engine {

TileMeshPreparationManager::TileMeshPreparationManager(
    TileContentLifecycleManager& contentLifecycle,
    TileContentResourceInvalidator& resourceInvalidator,
    TileLoadQueue& loadQueue,
    bool /*hasTerrainQuadtree*/,
    RenderDevice* /*device*/,
    const std::vector<ActivatedRasterOverlay*>& /*rasterOverlays*/)
    : contentLifecycle_(contentLifecycle),
      resourceInvalidator_(resourceInvalidator),
      loadQueue_(loadQueue) {}

void TileMeshPreparationManager::prepareRenderableTile(
    TilesetTile& tile,
    IPrepareRendererResources* pPrepRenderer) {
    prepareContentTerrainFrame(tile, pPrepRenderer);
}

void TileMeshPreparationManager::prepareContentTerrainFrame(
    TilesetTile& tile,
    IPrepareRendererResources* pPrepRenderer) {
    TileMeshFrameEnsurer::ensureContentTerrain(
        TileContentTerrainMeshFrameEnsureInput{
            tile,
            pPrepRenderer},
        [this]() {
            markResourcesDirty();
        });
}

bool TileMeshPreparationManager::prepareUpsampleSourceTile(
    TilesetTile& tile,
    double priority,
    IPrepareRendererResources* pPrepRenderer) {
    return TileUpsampleSourcePreparer::prepareSourceTile(
        tile,
        priority,
        false,
        [this, pPrepRenderer](TilesetTile& ancestor) {
            prepareRenderableTile(ancestor, pPrepRenderer);
        },
        [this](const TileKey& key,
               TileLoadPriorityGroup group,
               double queuePriority) {
            queueTileLoad(key, group, queuePriority);
        });
}

void TileMeshPreparationManager::markResourcesDirty() {
    resourceInvalidator_.markResourcesDirty();
}

void TileMeshPreparationManager::queueTileLoad(
    const TileKey& key,
    TileLoadPriorityGroup group,
    double priority) {
    loadQueue_.queue(key, group, priority);
}

} // namespace earth_engine
