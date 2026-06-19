#include "Tileset.h"
#include "../scene/FrameState.h"
#include "../scene/Camera.h"
#include "../renderer/Renderer.h"
#include "../renderer/RenderDevice.h"
#include "../tiling/TileFrameResourceBudgetPlanner.h"
#include "../tiling/TileRenderReferenceReleaser.h"
#include "../tiling/TilesetOcclusionFacade.h"
#include "../tiling/TilesetQueryFacade.h"
#include "../tiling/TilesetRenderFrameFacade.h"
#include "../tiling/TilesetUpdateFrameFacade.h"
#include "../layers/ActivatedRasterOverlay.h"

#include <memory>
#include <optional>
#include <utility>

namespace earth_engine {

namespace {

FrameResourceBudgetConfig makeFrameResourceBudgetConfig(
    const TilesetOptions& options,
    bool interactionActive,
    bool resourceSmoothingActive) {
    return TileFrameResourceBudgetPlanner::plan(
        TileFrameResourceBudgetPlanInput::withDefaultTransport(
            options.maximumSimultaneousTileLoads,
            options.mainThreadLoadingTimeLimit,
            interactionActive,
            resourceSmoothingActive));
}

} // namespace

Tileset::Tileset(std::unique_ptr<TerrainProvider> terrainProvider,
                 std::unique_ptr<TileScheme> tileScheme,
                 std::vector<ActivatedRasterOverlay*> rasterOverlays,
                 RenderDevice* device,
                 TilesetOptions options,
                 std::unique_ptr<TilesetContentProvider> contentProvider)
    : terrainProvider_(std::move(terrainProvider)),
      contentProvider_(std::move(contentProvider)),
      tileScheme_(std::move(tileScheme)),
      rasterOverlays_(std::move(rasterOverlays)),
      device_(device),
      options_(std::move(options)),
      contentAccess_(
          tileRegistry_,
          *tileScheme_,
          terrainProvider_.get(),
          contentProvider_.get(),
          contentLifecycle_,
          rasterOverlays_.size()),
      resourceInvalidator_(
          resourceRevision_,
          contentCache_,
          selectionReuseState_),
      cacheOwnership_(
          contentCache_,
          contentLifecycle_,
          loadQueue_,
          tileRegistry_.tiles(),
          resourceSmoothingActiveForFrame_,
          options_.maximumCachedBytes,
          options_.tileCacheUnloadTimeLimit),
      rasterUpsampledChildren_(
          contentAccess_,
          resourceInvalidator_),
      meshPreparation_(
          contentLifecycle_,
          resourceInvalidator_,
          loadQueue_,
          terrainProvider_.get(),
          device_,
          rasterOverlays_),
      renderCommands_(
          meshPreparation_,
          contentCache_,
          contentLifecycle_,
          rasterUpsampledChildren_,
          rasterOverlays_,
          device_,
          frameResourceBudget_) {
    frameResourceBudget_.beginFrame(
        0,
        makeFrameResourceBudgetConfig(options_, false, false));
    for (ActivatedRasterOverlay* overlay : rasterOverlays_) {
        if (overlay) {
            overlay->ensureTileProvider(device_);
        }
    }
}

Tileset::~Tileset() {
    // cesium-native keeps TilesetContentManager alive while worker callbacks
    // complete. This local engine has synchronous destruction, so wait until
    // every callback has observed the destroyed state and left the callback.
    contentLifecycle_.shutdown();
}

int Tileset::pendingRequests() const {
    return TilesetQueryFacade::pendingRequests(*this);
}

int Tileset::cachedTerrainTiles() const {
    return TilesetQueryFacade::cachedTerrainTiles(*this);
}

int64_t Tileset::totalBytesUsed() const {
    return TilesetQueryFacade::totalBytesUsed(*this);
}

void Tileset::setOcclusionCallback(OcclusionCallback callback) {
    TilesetOcclusionFacade::setOcclusionCallback(*this, std::move(callback));
}

void Tileset::clearOcclusionCallback() {
    TilesetOcclusionFacade::clearOcclusionCallback(*this);
}

TilesetLoadDiagnostics Tileset::loadDiagnostics() const {
    return TilesetQueryFacade::loadDiagnostics(*this);
}

float Tileset::sampleHeight(double lngRad, double latRad) const {
    return TilesetQueryFacade::sampleHeight(*this, lngRad, latRad);
}

void Tileset::update(const FrameState& frameState) {
    TilesetUpdateFrameFacade::update(*this, frameState);
}

void Tileset::buildRenderCommands(Renderer& renderer,
                                  RenderCommandList& commands) {
    TilesetRenderFrameFacade::buildRenderCommands(*this, renderer, commands);
}

void Tileset::releaseRenderReferences() {
    // cesium-native: called from Scene after renderer_->submit().
    // Drops the reference that was added in buildRenderCommands for
    // tiles in the current render list, making them eligible for
    // unloading in the next frame.
    TileRenderReferenceReleaser::release(tileRegistry_.tiles());
}

} // namespace earth_engine
