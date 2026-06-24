#include "TileMeshPreparationManager.h"

#include "TileCacheKey.h"
#include "TileContentLifecycleManager.h"
#include "TileContentResourceInvalidator.h"
#include "TileLoadQueue.h"
#include "TileMeshFrameEnsurer.h"
#include "RasterMappedToTilesetTile.h"
#include "TileSelectionRasterOverlayPreparer.h"
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
      hasTerrainQuadtree_(hasTerrainQuadtree),
      mode_(mode),
      device_(device),
      rasterOverlays_(rasterOverlays) {}

void TileMeshPreparationManager::prepareRenderableTile(TilesetTile& tile) {
    if (!usesLegacyHeightmapSurfacePath()) {
        prepareContentTerrainFrame(tile);
        return;
    }
    prepareHeightmapSurfaceFrame(tile);
}

void TileMeshPreparationManager::prepareContentTerrainFrame(TilesetTile& tile) {
    TileMeshFrameEnsurer::ensureContentTerrain(
        TileContentTerrainMeshFrameEnsureInput{
            tile},
        [this]() {
            markResourcesDirty();
        });
}

void TileMeshPreparationManager::prepareHeightmapSurfaceFrame(
    TilesetTile& tile) {
    auto ingestAvailability = [](const TileKey&, DecodedHeightmap*) {};
    auto findUpsampleSource =
        [](const TilesetTile& sourceTile, bool allowUnloadingSource) {
            return TileUpsampleSourcePreparer::findSourceTile(
                sourceTile,
                allowUnloadingSource,
                false,
                true);
        };
    auto ensureAncestorMesh = [this](TilesetTile& ancestor) {
        prepareRenderableTile(ancestor);
    };
    auto isCompleteRenderable = [this](const TilesetTile& renderableTile) {
        return TileSelectionRasterOverlayPreparer::isCompleteRenderable(
            renderableTile,
            rasterOverlays_);
    };
    auto markDirty = [this]() {
        markResourcesDirty();
    };

    TileMeshFrameEnsurer::ensureHeightmapSurface(
        TileHeightmapMeshFrameEnsureInput{
            tile,
            contentLifecycle_.legacyHeightmapTerrainCache(),
            device_,
            hasTerrainQuadtree_},
        [](const TileKey& key) {
            return TileCacheKey::forTile(key);
        },
        ingestAvailability,
        findUpsampleSource,
        ensureAncestorMesh,
        isCompleteRenderable,
        markDirty);
}

bool TileMeshPreparationManager::prepareUpsampleSourceTile(
    TilesetTile& tile,
    double priority) {
    return TileUpsampleSourcePreparer::prepareSourceTile(
        tile,
        priority,
        usesLegacyHeightmapSurfacePath(),
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
