#include "TileLegacyHeightmapSurfacePreparer.h"

#include "RasterMappedToTilesetTile.h"
#include "TileCacheKey.h"
#include "TileContentLifecycleManager.h"
#include "TileMeshFrameEnsurer.h"
#include "TileSelectionRasterOverlayPreparer.h"
#include "TileUpsampleSourcePreparer.h"
#include "TilesetTile.h"

#include <utility>

namespace earth_engine {

TileLegacyHeightmapSurfacePreparer::TileLegacyHeightmapSurfacePreparer(
    TileContentLifecycleManager& contentLifecycle,
    RenderDevice* device,
    const std::vector<ActivatedRasterOverlay*>& rasterOverlays,
    bool hasTerrainQuadtree,
    MarkDirty markDirty,
    QueueTileLoad queueTileLoad,
    PrepareRenderableTile prepareRenderableTile)
    : contentLifecycle_(contentLifecycle),
      device_(device),
      rasterOverlays_(rasterOverlays),
      hasTerrainQuadtree_(hasTerrainQuadtree),
      markDirty_(std::move(markDirty)),
      queueTileLoad_(std::move(queueTileLoad)),
      prepareRenderableTile_(std::move(prepareRenderableTile)) {}

void TileLegacyHeightmapSurfacePreparer::prepareRenderableTile(
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
    auto isCompleteRenderable = [this](const TilesetTile& renderableTile) {
        return TileSelectionRasterOverlayPreparer::isCompleteRenderable(
            renderableTile,
            rasterOverlays_);
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
        prepareRenderableTile_,
        isCompleteRenderable,
        markDirty_);
}

bool TileLegacyHeightmapSurfacePreparer::prepareUpsampleSourceTile(
    TilesetTile& tile,
    double priority) {
    return TileUpsampleSourcePreparer::prepareSourceTile(
        tile,
        priority,
        true,
        prepareRenderableTile_,
        queueTileLoad_);
}

} // namespace earth_engine
