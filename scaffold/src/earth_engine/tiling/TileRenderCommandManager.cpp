#include "TileRenderCommandManager.h"

#include "TileContentResourceInvalidator.h"
#include "TileMeshPreparationManager.h"
#include "TilesetTile.h"
#include "RasterOverlayRuntime.h"
#include "../renderer/Renderer.h"

namespace earth_engine {

TileRenderCommandManager::TileRenderCommandManager(
    TileMeshPreparationManager& meshPreparation,
    TileContentResourceInvalidator& resourceInvalidator,
    const RasterOverlayFrameContext& rasterFrame,
    RenderDevice* device)
    : meshPreparation_(meshPreparation),
      resourceInvalidator_(resourceInvalidator),
      rasterFrame_(rasterFrame),
      device_(device) {}

void TileRenderCommandManager::beginFrame(
    uint64_t frameNumber,
    uint64_t generation,
    double currentFrameTimeSeconds,
    const TerrainEdgeLutTableMap* edgeLutTables) {
    frameNumber_ = frameNumber;
    generation_ = generation;
    currentFrameTimeSeconds_ = currentFrameTimeSeconds;
    edgeLutTables_ = edgeLutTables;
    frameTimings_ = TileRenderCommandPerformanceTimings{};
}

void TileRenderCommandManager::buildTileDrawCommand(
    Renderer& renderer,
    TilesetTile& tile,
    RenderCommandList& commands,
    float transitionOpacity,
    bool allowSynchronousMeshPrep,
    const std::optional<std::array<float, 4>>& surfaceClipUv,
    const TilesetTile* surfaceClipDescendant) {
    const bool resourcesChanged = TileRenderCommandPreparer::build(
        renderer,
        tile,
        commands,
        device_,
        TileRenderCommandPrepareContext{
            rasterFrame_,
            frameNumber_,
            generation_,
            currentFrameTimeSeconds_,
            transitionOpacity,
            allowSynchronousMeshPrep,
            surfaceClipUv,
            surfaceClipDescendant,
            edgeLutTables_},
        [this, &renderer](TilesetTile& meshTile) {
            meshPreparation_.prepareRenderableTile(meshTile, &renderer);
        },
        &frameTimings_);
    if (resourcesChanged) {
        resourceInvalidator_.markTileResourcesChanged(tile);
    }
}

} // namespace earth_engine
