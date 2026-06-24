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
    bool hasTerrainQuadtree,
    TileMeshPreparationMode mode,
    RenderDevice* device,
    const std::vector<ActivatedRasterOverlay*>& rasterOverlays)
    : contentLifecycle_(contentLifecycle),
      resourceInvalidator_(resourceInvalidator),
      loadQueue_(loadQueue),
      mode_(mode) {
    if (usesLegacyHeightmapSurfacePath()) {
        legacyHeightmapSurfacePreparer_ =
            std::make_unique<TileLegacyHeightmapSurfacePreparer>(
                contentLifecycle_,
                device,
                rasterOverlays,
                hasTerrainQuadtree,
                [this]() {
                    markResourcesDirty();
                },
                [this](const TileKey& key,
                       TileLoadPriorityGroup group,
                       double queuePriority) {
                    queueTileLoad(key, group, queuePriority);
                },
                [this](TilesetTile& ancestor) {
                    prepareRenderableTile(ancestor);
                });
    }
}

void TileMeshPreparationManager::prepareRenderableTile(TilesetTile& tile) {
    if (!usesLegacyHeightmapSurfacePath()) {
        prepareContentTerrainFrame(tile);
        return;
    }
    legacyHeightmapSurfacePreparer_->prepareRenderableTile(tile);
}

void TileMeshPreparationManager::prepareContentTerrainFrame(TilesetTile& tile) {
    TileMeshFrameEnsurer::ensureContentTerrain(
        TileContentTerrainMeshFrameEnsureInput{
            tile},
        [this]() {
            markResourcesDirty();
        });
}

bool TileMeshPreparationManager::prepareUpsampleSourceTile(
    TilesetTile& tile,
    double priority) {
    if (usesLegacyHeightmapSurfacePath()) {
        return legacyHeightmapSurfacePreparer_->prepareUpsampleSourceTile(
            tile,
            priority);
    }

    return TileUpsampleSourcePreparer::prepareSourceTile(
        tile,
        priority,
        false,
        [this](TilesetTile& ancestor) {
            prepareRenderableTile(ancestor);
        },
        [this](const TileKey& key,
               TileLoadPriorityGroup group,
               double queuePriority) {
            queueTileLoad(key, group, queuePriority);
        });
}

bool TileMeshPreparationManager::usesLegacyHeightmapSurfacePath() const {
    return mode_ == TileMeshPreparationMode::LegacyHeightmapSurface;
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
