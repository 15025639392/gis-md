#include "Tileset.h"
#include "../scene/FrameState.h"
#include "../scene/Camera.h"
#include "../renderer/Renderer.h"
#include "../renderer/RenderDevice.h"
#include "../tiling/LoadedTerrainHeightSampler.h"
#include "../tiling/TileFrameResourceBudgetPlanner.h"
#include "../tiling/TileOcclusionResolver.h"
#include "../tiling/TileRenderFrameContext.h"
#include "../tiling/TileRenderReferenceReleaser.h"
#include "../tiling/TileSoftwareOcclusionPolicy.h"
#include "../tiling/TilesetProviderDiagnosticsCollector.h"
#include "../tiling/TilesetRenderFrameExecutor.h"
#include "../tiling/TilesetUpdateFrameFacade.h"
#include "../layers/ActivatedRasterOverlay.h"

#include <memory>
#include <optional>
#include <utility>

namespace earth_engine {

namespace {

constexpr int kSmoothedMainThreadUploadLimit = 1;

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
          contentCache_),
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
      contentRuntime_(
          contentLifecycle_,
          contentAccess_,
          meshPreparation_,
          resourceInvalidator_),
      renderCommands_(
          meshPreparation_,
          cacheOwnership_,
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
    return contentLifecycle_.pendingRequests();
}

int Tileset::cachedTerrainTiles() const {
    return static_cast<int>(contentLifecycle_.terrainCache().size());
}

int64_t Tileset::totalBytesUsed() const {
    return contentCache_.totalBytesUsed();
}

void Tileset::setOcclusionCallback(OcclusionCallback callback) {
    occlusionCallback_ = std::move(callback);
}

void Tileset::clearOcclusionCallback() {
    occlusionCallback_ = nullptr;
}

TilesetLoadDiagnostics Tileset::loadDiagnostics() const {
    TilesetLoadDiagnostics diagnostics = TileLoadDiagnosticsCollector::collect(
        loadQueue_,
        contentLifecycle_.loadLifecycle(),
        frameResourceBudget_,
        contentCache_.unloadQueue(),
        tileRegistry_.tiles());
    TilesetProviderDiagnosticsCollector::collect(
        terrainProvider_.get(),
        contentProvider_.get(),
        rasterOverlays_)
        .applyTo(diagnostics);
    return diagnostics;
}

TileContentRuntimeFrame Tileset::makeContentRuntimeFrame() const {
    return TileContentRuntimeFrame{
        terrainProvider_.get(),
        contentProvider_.get(),
        device_,
        tileRegistry_.tiles(),
        frameNumber_,
        options_.maximumSimultaneousTileLoads,
        options_.mainThreadLoadingTimeLimit,
        currentFrameTimeSeconds_,
        static_cast<uint32_t>(kSmoothedMainThreadUploadLimit)};
}

TileLoadRequestOutcome Tileset::requestMissingContent(
    const std::vector<TileLoadRequest>& loadRequests,
    FrameResourceBudget* budget) {
    return contentRuntime_.requestMissingTiles(
        loadRequests,
        makeContentRuntimeFrame(),
        budget);
}

bool Tileset::processPendingLoads(
    bool interactionActive,
    bool resourceSmoothingActive,
    FrameResourceBudget* budget) {
    return contentRuntime_.processPendingUploads(
        makeContentRuntimeFrame(),
        interactionActive,
        resourceSmoothingActive,
        budget);
}

void Tileset::markContentResourcesDirty() {
    contentRuntime_.markResourcesDirty();
}

TileOcclusionState Tileset::checkSingleTileOcclusion(
    const TilesetTile& tile) const {
    if (occlusionCallback_) {
        return occlusionCallback_(tile);
    }
    return TileSoftwareOcclusionPolicy::check(tile, lastCameraPosition_);
}

TileOcclusionState Tileset::checkOcclusion(const TilesetTile& tile) const {
    return TileOcclusionResolver::check(
        tile,
        [this](const TilesetTile& occlusionTile) {
            return checkSingleTileOcclusion(occlusionTile);
        });
}

float Tileset::sampleHeight(double lngRad, double latRad) const {
    return LoadedTerrainHeightSampler::sampleHeight(
        tileRegistry_.tiles(),
        contentLifecycle_.terrainCache(),
        lngRad,
        latRad);
}

void Tileset::update(const FrameState& frameState) {
    TilesetUpdateFrameFacade::update(*this, frameState);
}

void Tileset::buildRenderCommands(Renderer& renderer,
                                  RenderCommandList& commands) {
    ++frameNumber_;
    renderCommands_.beginFrame(
        frameNumber_,
        generation_,
        currentFrameTimeSeconds_,
        options_.maximumScreenSpaceError);
    TilesetRenderFrameExecutor::buildRenderCommands(
        TileRenderFrameContext{
            TileRenderFrameCoordinatorInput{
                tilePlan_,
                tileRegistry_.tiles(),
                contentCache_.unloadQueue(),
                rasterOverlays_,
                contentCache_.cacheBytesDirty(),
                frameNumber_,
                lastCameraPosition_,
                options_.fogDensityTable,
                selectionCounters_.fogCulled,
                resourceSmoothingActiveForFrame_,
                interactionActiveForFrame_,
                contentCache_.totalBytesUsed(),
                options_.maximumCachedBytes},
            contentAccess_,
            renderCommands_,
            cacheOwnership_},
        renderer,
        commands);
}

void Tileset::releaseRenderReferences() {
    // cesium-native: called from Scene after renderer_->submit().
    // Drops the reference that was added in buildRenderCommands for
    // tiles in the current render list, making them eligible for
    // unloading in the next frame.
    TileRenderReferenceReleaser::release(tileRegistry_.tiles());
}

} // namespace earth_engine
