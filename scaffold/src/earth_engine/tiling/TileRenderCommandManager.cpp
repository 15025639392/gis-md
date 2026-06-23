#include "TileRenderCommandManager.h"

#include "TileCacheOwnershipManager.h"
#include "TileMeshPreparationManager.h"
#include "RasterMappedToTilesetTile.h"
#include "TileRasterUpsampledChildCoordinator.h"
#include "../providers/RasterOverlayTile.h"
#include "../providers/RasterOverlayTileProvider.h"
#include "TileSelectionReuseState.h"
#include "TilesetTile.h"
#include "../renderer/Renderer.h"

namespace earth_engine {

TileRenderCommandManager::TileRenderCommandManager(
    TileMeshPreparationManager& meshPreparation,
    TileCacheOwnershipManager& cacheOwnership,
    TileRasterUpsampledChildCoordinator& rasterUpsampledChildren,
    const std::vector<ActivatedRasterOverlay*>& rasterOverlays,
    RenderDevice* device,
    FrameResourceBudget& frameResourceBudget)
    : meshPreparation_(meshPreparation),
      cacheOwnership_(cacheOwnership),
      rasterUpsampledChildren_(rasterUpsampledChildren),
      rasterOverlays_(rasterOverlays),
      device_(device),
      frameResourceBudget_(frameResourceBudget) {}

void TileRenderCommandManager::beginFrame(
    uint64_t frameNumber,
    uint64_t generation,
    double currentFrameTimeSeconds,
    double maximumScreenSpaceError) {
    frameNumber_ = frameNumber;
    generation_ = generation;
    currentFrameTimeSeconds_ = currentFrameTimeSeconds;
    maximumScreenSpaceError_ = maximumScreenSpaceError;
}

void TileRenderCommandManager::buildTileDrawCommand(
    Renderer& renderer,
    TilesetTile& tile,
    RenderCommandList& commands,
    float transitionOpacity,
    bool allowSynchronousMeshPrep,
    const std::optional<std::array<float, 4>>& surfaceClipUv) {
    TileRenderCommandPreparer::build(
        renderer,
        tile,
        commands,
        rasterOverlays_,
        device_,
        frameResourceBudget_,
        TileRenderCommandPrepareContext{
            frameNumber_,
            generation_,
            currentFrameTimeSeconds_,
            maximumScreenSpaceError_,
            transitionOpacity,
            allowSynchronousMeshPrep,
            surfaceClipUv},
        [this](TilesetTile& meshTile) {
            meshPreparation_.prepareRenderableTile(meshTile);
        },
        [this, &renderer](TilesetTile& unloadTile) {
            cacheOwnership_.unloadTileContent(unloadTile, &renderer);
        },
        [this, &renderer](TilesetTile& upsampleTile) {
            rasterUpsampledChildren_.createRasterOverlayUpsampledChildren(
                upsampleTile,
                &renderer);
        });
}

} // namespace earth_engine
