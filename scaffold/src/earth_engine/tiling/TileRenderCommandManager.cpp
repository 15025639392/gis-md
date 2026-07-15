#include "TileRenderCommandManager.h"

#include "TileContentResourceInvalidator.h"
#include "TileMeshPreparationManager.h"
#include "TilesetTile.h"
#include "../renderer/Renderer.h"

namespace earth_engine {

TileRenderCommandManager::TileRenderCommandManager(
    TileMeshPreparationManager& meshPreparation,
    TileContentResourceInvalidator& resourceInvalidator,
    const std::vector<ActivatedRasterOverlay*>& rasterOverlays,
    RenderDevice* device)
    : meshPreparation_(meshPreparation),
      resourceInvalidator_(resourceInvalidator),
      rasterOverlays_(rasterOverlays),
      device_(device) {}

void TileRenderCommandManager::beginFrame(
    uint64_t frameNumber,
    uint64_t generation,
    double currentFrameTimeSeconds) {
    frameNumber_ = frameNumber;
    generation_ = generation;
    currentFrameTimeSeconds_ = currentFrameTimeSeconds;
    frameTimings_ = TileRenderCommandPerformanceTimings{};
}

void TileRenderCommandManager::buildTileDrawCommand(
    Renderer& renderer,
    TilesetTile& tile,
    RenderCommandList& commands,
    float transitionOpacity,
    bool allowSynchronousMeshPrep,
    const std::optional<std::array<float, 4>>& surfaceClipUv) {
    const bool resourcesChanged = TileRenderCommandPreparer::build(
        renderer,
        tile,
        commands,
        rasterOverlays_,
        device_,
        TileRenderCommandPrepareContext{
            frameNumber_,
            generation_,
            currentFrameTimeSeconds_,
            transitionOpacity,
            allowSynchronousMeshPrep,
            surfaceClipUv},
        [this, &renderer](TilesetTile& meshTile) {
            meshPreparation_.prepareRenderableTile(meshTile, &renderer);
        },
        &frameTimings_);
    if (resourcesChanged) {
        resourceInvalidator_.markTileResourcesChanged(tile);
    }
}

} // namespace earth_engine
