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

bool providerHasTerrainQuadtree(const TerrainProvider* terrainProvider,
                                const TilesetContentProvider* contentProvider) {
    return terrainProvider != nullptr ||
           (contentProvider && contentProvider->providesTerrainQuadtree());
}

bool contentProviderOwnsTerrainQuadtree(
    const TilesetContentProvider* contentProvider) {
    return contentProvider && contentProvider->providesTerrainQuadtree();
}

bool usesHeightmapTerrainSurfacePath(const TerrainProvider* terrainProvider) {
    return terrainProvider != nullptr;
}

} // namespace

Tileset::ProviderOwnership Tileset::ProviderOwnership::noTerrain() {
    return ProviderOwnership{};
}

Tileset::ProviderOwnership Tileset::ProviderOwnership::contentTerrain(
    std::unique_ptr<TilesetContentProvider> contentProvider) {
    ProviderOwnership providers;
    providers.contentProvider = std::move(contentProvider);
    return providers;
}

Tileset::ProviderOwnership
Tileset::ProviderOwnership::legacyHeightmapSurfaceForTests(
    std::unique_ptr<TerrainProvider> terrainProvider) {
    ProviderOwnership providers;
    providers.heightmapTerrainProvider = std::move(terrainProvider);
    return providers;
}

Tileset Tileset::createLegacyTerrainForTests(
    std::unique_ptr<TerrainProvider> terrainProvider,
    std::unique_ptr<TileScheme> tileScheme,
    std::vector<ActivatedRasterOverlay*> rasterOverlays,
    RenderDevice* device,
    TilesetOptions options) {
    return Tileset(
        ProviderOwnership::legacyHeightmapSurfaceForTests(
            std::move(terrainProvider)),
        std::move(tileScheme),
        std::move(rasterOverlays),
        device,
        std::move(options));
}

Tileset::Tileset(std::unique_ptr<TileScheme> tileScheme,
                 std::vector<ActivatedRasterOverlay*> rasterOverlays,
                 RenderDevice* device,
                 TilesetOptions options)
    : Tileset(
          ProviderOwnership::noTerrain(),
          std::move(tileScheme),
          std::move(rasterOverlays),
          device,
          std::move(options)) {}

Tileset::Tileset(ProviderOwnership providers,
                 std::unique_ptr<TileScheme> tileScheme,
                 std::vector<ActivatedRasterOverlay*> rasterOverlays,
                 RenderDevice* device,
                 TilesetOptions options)
    : heightmapTerrainProvider_(
          !contentProviderOwnsTerrainQuadtree(providers.contentProvider.get())
              ? std::move(providers.heightmapTerrainProvider)
              : std::unique_ptr<TerrainProvider>{}),
      contentProvider_(std::move(providers.contentProvider)),
      tileScheme_(std::move(tileScheme)),
      rasterOverlays_(std::move(rasterOverlays)),
      device_(device),
      options_(std::move(options)),
      contentAccess_(
          contentProviderOwnsTerrainQuadtree(contentProvider_.get())
              ? TileContentAccess::forContentTerrain(
                    tileRegistry_,
                    *tileScheme_,
                    *contentProvider_,
                    rasterOverlays_.size())
              : heightmapTerrainProvider_
                    ? TileContentAccess::forHeightmapTerrainSurfacePath(
                          tileRegistry_,
                          *tileScheme_,
                          heightmapTerrainProvider_.get(),
                          contentProvider_.get(),
                          contentLifecycle_.heightmapTerrainCache(),
                          rasterOverlays_.size())
                    : TileContentAccess::forNoTerrain(
                          tileRegistry_,
                          *tileScheme_,
                          contentProvider_.get(),
                          rasterOverlays_.size())),
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
          options_.tileCacheUnloadTimeLimit,
          usesHeightmapTerrainSurfacePath(heightmapTerrainProvider_.get())),
      rasterUpsampledChildren_(
          contentAccess_,
          resourceInvalidator_),
      meshPreparation_(
          contentLifecycle_,
          resourceInvalidator_,
          loadQueue_,
          providerHasTerrainQuadtree(
              heightmapTerrainProvider_.get(),
              contentProvider_.get()),
          usesHeightmapTerrainSurfacePath(heightmapTerrainProvider_.get()),
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

Tileset::Tileset(
    std::unique_ptr<TileScheme> tileScheme,
    std::vector<ActivatedRasterOverlay*> rasterOverlays,
    RenderDevice* device,
    TilesetOptions options,
    std::unique_ptr<TilesetContentProvider> contentProvider)
    : Tileset(
          ProviderOwnership::contentTerrain(std::move(contentProvider)),
          std::move(tileScheme),
          std::move(rasterOverlays),
          device,
          std::move(options)) {}

bool Tileset::hasTerrainQuadtree() const {
    return providerHasTerrainQuadtree(
        heightmapTerrainProvider_.get(),
        contentProvider_.get());
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

int Tileset::cachedHeightmapTerrainTilesForLegacySurfacePath() const {
    if (!usesHeightmapTerrainSurfacePath(heightmapTerrainProvider_.get())) {
        return 0;
    }
    return static_cast<int>(contentLifecycle_.heightmapTerrainCache().size());
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
        legacyHeightmapTerrainProviderForSurfacePath(),
        contentProvider_.get(),
        rasterOverlays_)
        .applyTo(diagnostics);
    return diagnostics;
}

const TerrainProvider* Tileset::legacyHeightmapTerrainProviderForSurfacePath()
    const {
    return heightmapTerrainProvider_.get();
}

TileContentRuntimeRequestFrame
Tileset::makeContentRuntimeRequestFrame() const {
    TileContentRuntimeRequestFrame frame{
        rasterOverlays_,
        tileRegistry_.tiles()};
    frame.contentProvider = contentProvider_.get();
    frame.device = device_;
    frame.frameNumber = frameNumber_;
    frame.maximumSimultaneousTileLoads =
        options_.maximumSimultaneousTileLoads;
    frame.mainThreadLoadingTimeLimit = options_.mainThreadLoadingTimeLimit;
    frame.currentFrameTimeSeconds = currentFrameTimeSeconds_;
    frame.smoothedMainThreadUploadLimit =
        static_cast<uint32_t>(kSmoothedMainThreadUploadLimit);
    return frame;
}

TileContentRuntimeUploadFrame Tileset::makeContentRuntimeUploadFrame(
    IPrepareRendererResources* pPrepRenderer) const {
    TileContentRuntimeUploadFrame frame{rasterOverlays_};
    frame.contentProvider = contentProvider_.get();
    frame.device = device_;
    frame.pPrepRenderer = pPrepRenderer;
    frame.frameNumber = frameNumber_;
    frame.maximumSimultaneousTileLoads =
        options_.maximumSimultaneousTileLoads;
    frame.mainThreadLoadingTimeLimit = options_.mainThreadLoadingTimeLimit;
    frame.currentFrameTimeSeconds = currentFrameTimeSeconds_;
    frame.smoothedMainThreadUploadLimit =
        static_cast<uint32_t>(kSmoothedMainThreadUploadLimit);
    return frame;
}

TileLoadRequestOutcome Tileset::requestMissingContent(
    const std::vector<TileLoadRequest>& loadRequests,
    FrameResourceBudget* budget) {
    return contentRuntime_.requestMissingTiles(
        loadRequests,
        makeContentRuntimeRequestFrame(),
        budget);
}

bool Tileset::processPendingLoads(
    bool interactionActive,
    bool resourceSmoothingActive,
    IPrepareRendererResources* pPrepRenderer,
    FrameResourceBudget* budget) {
    return contentRuntime_.processPendingUploads(
        makeContentRuntimeUploadFrame(pPrepRenderer),
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
        contentLifecycle_.heightmapTerrainCache(),
        lngRad,
        latRad,
        usesHeightmapTerrainSurfacePath(heightmapTerrainProvider_.get()));
}

void Tileset::update(
    const FrameState& frameState,
    IPrepareRendererResources* pPrepRenderer) {
    TilesetUpdateFrameFacade::update(*this, frameState, pPrepRenderer);
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
