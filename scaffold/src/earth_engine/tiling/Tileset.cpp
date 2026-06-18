#include "Tileset.h"
#include "../scene/FrameState.h"
#include "../scene/Camera.h"
#include "../renderer/Renderer.h"
#include "../renderer/RenderDevice.h"
#include "../core/geodesy/Ellipsoid.h"
#include "../core/geodesy/Cartographic.h"
#include "../tiling/LoadedTerrainHeightSampler.h"
#include "../tiling/GltfRenderResourcePreparer.h"
#include "../tiling/TileChildFrameMaterializer.h"
#include "../tiling/TileChildMaterializer.h"
#include "../tiling/TileCacheKey.h"
#include "../tiling/TileContentUnloadCoordinator.h"
#include "../tiling/TileCacheUnloadCoordinator.h"
#include "../tiling/TileCreationPolicy.h"
#include "../tiling/TileFrameBudgetFallback.h"
#include "../tiling/TileFrameDebugLogFormatter.h"
#include "../tiling/TileFrameInteractionTracker.h"
#include "../tiling/TileFrameResourceBudgetPlanner.h"
#include "../tiling/TileLodTransitionController.h"
#include "../tiling/TileMeshFrameEnsurer.h"
#include "../tiling/TileMissingRequestScheduler.h"
#include "../tiling/TileOcclusionResolver.h"
#include "../tiling/TilePendingUploadFrameProcessor.h"
#include "../tiling/TileQuantizedMeshAvailabilityIngestor.h"
#include "../tiling/TileRasterOverlaySignature.h"
#include "../tiling/TileRasterUpsampledChildMaterializer.h"
#include "../tiling/TileRefinementAvailabilityResolver.h"
#include "../tiling/TileRenderCommandPreparer.h"
#include "../tiling/TileRenderFrameBuilder.h"
#include "../tiling/TileRenderFrameInputBuilder.h"
#include "../tiling/TileRenderPlanFinalizer.h"
#include "../tiling/TileRenderReferenceReleaser.h"
#include "../tiling/TileRenderablePolicy.h"
#include "../tiling/TileSelectionChildTraversal.h"
#include "../tiling/TileSelectionCullingPolicy.h"
#include "../tiling/TileSelectionFrameFinalizer.h"
#include "../tiling/TileSelectionFrameRunner.h"
#include "../tiling/TileSelectionHistory.h"
#include "../tiling/TileSelectionPostTraversalCommitter.h"
#include "../tiling/TileSelectionPostTraversalPolicy.h"
#include "../tiling/TileSelectionPreTraversalPolicy.h"
#include "../tiling/TileSelectionRefineFlowPolicy.h"
#include "../tiling/TileSelectionRenderEntryPolicy.h"
#include "../tiling/TileSelectionResetPolicy.h"
#include "../tiling/TileSelectionReusePolicy.h"
#include "../tiling/TileSelectionTraversalCounterPolicy.h"
#include "../tiling/TileSelectionRasterOverlayPreparer.h"
#include "../tiling/TileSelectionVisitPreparation.h"
#include "../tiling/TileSoftwareOcclusionPolicy.h"
#include "../tiling/TileSurface.h"
#include "../tiling/TileSubtreeRemovalCoordinator.h"
#include "../tiling/TileSubtreeWorkTracker.h"
#include "../tiling/TileUpsampleSourcePreparer.h"
#include "../tiling/TileUpdateFrameContextBuilder.h"
#include "../tiling/TileUpdateSelectionWorkRunner.h"
#include "../tiling/TileUpdateUploadRunner.h"
#include "../providers/QuantizedMeshTerrainProvider.h"
#include "../providers/RasterOverlayTileProvider.h"
#include "../terrain/TerrainTile.h"
#include "../layers/ActivatedRasterOverlay.h"
#include "../layers/RasterOverlay.h"
#include "../debug/PerfTimer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <utility>

namespace earth_engine {

namespace {

constexpr double kTerrainMapQuality = 0.25;
constexpr double kTerrainMapWidth = 65.0;
constexpr double kPostInteractionResourceSmoothingSeconds = 1.25;
constexpr int kSmoothedMainThreadUploadLimit = 1;
constexpr int kActiveInteractionRenderPrepBudget = 0;
constexpr int kRecoveryRenderPrepBudget = 1;

FrameResourceBudgetConfig makeFrameResourceBudgetConfig(
    const TilesetOptions& options,
    bool interactionActive,
    bool resourceSmoothingActive) {
    return TileFrameResourceBudgetPlanner::plan(
        TileFrameResourceBudgetPlanInput{
            options.maximumSimultaneousTileLoads,
            options.mainThreadLoadingTimeLimit,
            interactionActive,
            resourceSmoothingActive});
}

double cesiumTerrainGeometricError(const Rectangle& bounds) {
    // cesium-native LayerJsonTerrainLoader:
    // 8.0 * calcQuadtreeMaxGeometricError(ellipsoid) * rectangle.computeWidth()
    const double maxGeometricErrorPerRadian =
        Ellipsoid::WGS84().semiMajorAxis() * kTerrainMapQuality / kTerrainMapWidth;
    return 8.0 * maxGeometricErrorPerRadian * bounds.width();
}

void kickVisitedDescendants(TilesetTile& tile) {
    for (TilesetTile* child : tile.children) {
        if (!child) continue;
        kickSelectionState(child->selectionState);
        kickVisitedDescendants(*child);
    }
}

bool wasRenderedLastFrameForTraversalDetails(const TilesetTile& tile) {
    return TileTraversalDetailsPolicy::wasRenderedLastFrameForTraversalDetails(
        tile.previousSelectionState,
        tile.refine,
        TileSelectionHistory::anyDescendantWasRenderedLastFrame(tile));
}

} // namespace

// ── cesium-native cache implementation ──

void Tileset::updateTotalBytesUsed() {
    // cesium-native: recompute total bytes from current tile state.
    // More robust than incremental tracking because overlay textures
    // attach/detach asynchronously, and terrain cache grows independently.
    totalBytesUsed_ =
        TileCacheMetrics::estimateTotalBytes(tiles_, terrainCache_);
}

void Tileset::markEligibleForUnloading(const std::string& key) {
    // cesium-native: add to back of LRU queue if not already present
    TileIndexState::markEligibleForUnloading(unloadQueue_, tiles_, key);
}

void Tileset::markIneligibleForUnloading(const std::string& key) {
    // cesium-native: remove from unload queue (tile was used this frame)
    TileIndexState::markIneligibleForUnloading(unloadQueue_, key);
}

bool Tileset::subtreeHasActiveContentWork(const TilesetTile& tile) {
    return TileSubtreeWorkTracker::hasActiveContentWork(
        tile,
        loadLifecycle_,
        [](const TileKey& key) {
            return TileCacheKey::forTile(key);
        });
}

void Tileset::eraseTileIndexState(const std::string& key) {
    TileIndexState::eraseCacheKeyState(
        key,
        unloadQueue_,
        terrainCache_,
        emptyContentRegistry_,
        loadQueue_,
        loadLifecycle_,
        [](const TileKey& tileKey) {
            return TileCacheKey::forTile(tileKey);
        });
}

void Tileset::clearChildrenRecursively(TilesetTile* tile,
                                        IPrepareRendererResources* pPrepRenderer) {
    TileSubtreeRemovalCoordinator::clearChildrenRecursively(
        tile,
        tiles_,
        pPrepRenderer,
        [](const TileKey& key) {
            return TileCacheKey::forTile(key);
        },
        [this](const std::string& cacheKey) {
            eraseTileIndexState(cacheKey);
        });
}

TileCacheUnloadContentResult Tileset::unloadTileContent(
    TilesetTile& tile,
    IPrepareRendererResources* pPrepRenderer) {
    return TileContentUnloadCoordinator::unloadContent(
        tile,
        TileCacheKey::forTile(tile.key),
        terrainCache_,
        emptyContentRegistry_,
        pPrepRenderer);
}

void Tileset::unloadCachedBytes(int64_t maximumCachedBytes,
                               IPrepareRendererResources* pPrepRenderer) {
    // cesium-native: unload from head of LRU queue until under budget
    const TileCacheUnloadResult result =
        TileCacheUnloadCoordinator::run(
            unloadQueue_,
            tiles_,
            totalBytesUsed_,
            maximumCachedBytes,
            options_.tileCacheUnloadTimeLimit,
            resourceSmoothingActiveForFrame_,
            cacheBytesDirty_,
            [this](const TilesetTile& tile) {
                // cesium-native: skip external subtrees with active content
                // work; native child references propagate to parents, while
                // this flat-map store checks the subtree explicitly.
                return subtreeHasActiveContentWork(tile);
            },
            [this, pPrepRenderer](TilesetTile& tile) {
                return unloadTileContent(tile, pPrepRenderer);
            },
            [this](const std::string& key) {
                markIneligibleForUnloading(key);
            },
            [this, pPrepRenderer](TilesetTile& tile) {
                clearChildrenRecursively(&tile, pPrepRenderer);
            });
    totalBytesUsed_ = result.totalBytesUsed;
    cacheBytesDirty_ = result.cacheBytesDirty;

    if (result.shouldRefreshTotalBytes) {
        updateTotalBytesUsed();
        cacheBytesDirty_ = false;
    }
}

// ──────────────────────────────────────────────────────────────

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
      options_(std::move(options)) {
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
    loadLifecycle_.markDestroyingCancelAndWait();
}

int Tileset::pendingRequests() const {
    return loadLifecycle_.pendingRequestCount();
}

void Tileset::setOcclusionCallback(OcclusionCallback callback) {
    occlusionCallback_ = std::move(callback);
}

void Tileset::clearOcclusionCallback() {
    occlusionCallback_ = nullptr;
}

TilesetLoadDiagnostics Tileset::loadDiagnostics() const {
    return TileLoadDiagnosticsCollector::collect(
        loadQueue_,
        loadLifecycle_,
        unloadQueue_,
        tiles_);
}

void Tileset::markTileResourcesDirty() {
    ++resourceRevision_;
    cacheBytesDirty_ = true;
    selectionReuseState_.invalidate();
}

bool Tileset::hasTilesetPendingWork() const {
    return loadLifecycle_.hasPendingWork();
}

float Tileset::sampleHeight(double lngRad, double latRad) const {
    return LoadedTerrainHeightSampler::sampleHeight(
        tiles_,
        terrainCache_,
        lngRad,
        latRad);
}

TilesetTile* Tileset::ensureTile(const TileKey& key) {
    const std::string ck = TileCacheKey::forTile(key);
    const std::optional<TilesetContentTileMetadata> contentMetadata =
        contentProvider_ ? contentProvider_->tileMetadata(key) : std::nullopt;
    auto it = tiles_.find(ck);
    if (it != tiles_.end() && it->second) {
        if (contentMetadata) {
            TileCreationPolicy::applyContentMetadata(
                *it->second,
                *contentMetadata);
        }
        return it->second.get();
    }

    TilesetTile* parent = nullptr;
    if (contentMetadata && contentMetadata->parentKey) {
        parent = ensureTile(*contentMetadata->parentKey);
    } else if (!contentMetadata && key.z > 0) {
        parent = ensureTile(TilePlanBuilder::parentKey(key));
    }

    const Rectangle bounds = contentMetadata && contentMetadata->hasExplicitBounds
        ? contentMetadata->bounds
        : tileScheme_->tileToRectangle(key);
    auto tile = std::make_unique<TilesetTile>(
        key, bounds, parent);
    TileCreationPolicy::initializeNewTile(
        *tile,
        contentMetadata,
        parent,
        cesiumTerrainGeometricError(tile->bounds),
        rasterOverlays_.size());

    TilesetTile* raw = tile.get();
    tiles_[ck] = std::move(tile);

    if (parent) {
        auto& children = parent->children;
        if (std::find(children.begin(), children.end(), raw) == children.end()) {
            children.push_back(raw);
        }
    }

    return raw;
}

void Tileset::resetTileSelectionState() {
    for (auto& [ck, tile] : tiles_) {
        if (!tile) continue;
        const TileSelectionResetPlan resetPlan =
            TileSelectionResetPolicy::plan(
                TileSelectionResetInput{
                    tile->selectionState,
                    hasSurfaceDrawable(*tile),
                    TileSelectionRasterOverlayPreparer::isCompleteRenderable(
                        *tile,
                        rasterOverlays_)});
        tile->previousSelectionState = resetPlan.previousSelectionState;
        tile->selectionState = resetPlan.selectionState;
        tile->screenSpaceError = resetPlan.screenSpaceError;
        tile->inFrustum = resetPlan.inFrustum;
        tile->cameraInside = resetPlan.cameraInside;
        tile->ancestorMeetsSse = resetPlan.ancestorMeetsSse;
        tile->surfaceDrawable = resetPlan.surfaceDrawable;
        tile->completeRenderable = resetPlan.completeRenderable;
        tile->renderable = resetPlan.renderable;
        (void)ck;
    }
}

bool Tileset::hasSurfaceDrawable(const TilesetTile& tile) const {
    return TileRenderablePolicy::hasSurfaceDrawable(
        tile.contentKind,
        tile.loadState,
        tile.meshReady,
        tile.gpuVertexBuffer != nullptr);
}

bool Tileset::hasLoadedTerrainContent(const TilesetTile& tile) const {
    auto it = terrainCache_.find(TileCacheKey::forTile(tile.key));
    return it != terrainCache_.end() && it->second != nullptr;
}

bool Tileset::isAvailabilityBoundaryTile(const TilesetTile& tile) const {
    auto* qmProvider = dynamic_cast<const QuantizedMeshTerrainProvider*>(
        terrainProvider_.get());
    if (!qmProvider) {
        return false;
    }
    return qmProvider->isAvailabilityBoundaryLevel(tile.key.z);
}

bool Tileset::prepareUpsampleSourceTile(TilesetTile& tile, double priority) {
    return TileUpsampleSourcePreparer::prepareSourceTile(
        tile,
        priority,
        [this](TilesetTile& ancestor) {
            ensureTileMesh(ancestor);
        },
        [this](const TileKey& key,
               TileLoadPriorityGroup group,
               double queuePriority) {
            queueTileLoad(key, group, queuePriority);
        });
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

TileTraversalDetails
Tileset::createTraversalDetailsForSingleTile(const TilesetTile& tile) const {
    const bool renderable = TileSelectionRasterOverlayPreparer::isRenderable(
        tile,
        rasterOverlays_);

    return TileTraversalDetailsPolicy::forSingleTile(
        renderable,
        wasRenderedLastFrameForTraversalDetails(tile));
}

TileTraversalDetails
Tileset::createTraversalDetailsForCulledTile(const TilesetTile& tile) const {
    if (!options_.forbidHoles || tile.refine != TileRefine::Replace) {
        return TileTraversalDetails{};
    }

    const bool renderable = TileSelectionRasterOverlayPreparer::isRenderable(
        tile,
        rasterOverlays_);
    return TileTraversalDetailsPolicy::forCulledTile(
        options_.forbidHoles,
        tile.refine,
        renderable,
        wasRenderedLastFrameForTraversalDetails(tile));
}

void Tileset::queueTileLoad(const TileKey& key,
                            TileLoadPriorityGroup group,
                            double priority) {
    loadQueue_.queue(key, group, priority);
}

void Tileset::addTileToCurrentPlan(TilesetTile& tile,
                                   double tileSse,
                                   bool queueForLoad,
                                   double tilePriority) {
    const TileSelectionRenderEntryPlan renderEntry =
        TileSelectionRenderEntryPolicy::plan(
            TileSelectionRenderEntryInput{
                options_.enableLodTransitionPeriod,
                queueForLoad});
    if (renderEntry.writeSelectionState) {
        tile.selectionState = renderEntry.selectionState;
    }
    if (renderEntry.writeScreenSpaceError) {
        tile.screenSpaceError = tileSse;
    }
    if (renderEntry.resetLodTransitionFade) {
        tile.lodTransitionFadePercentage =
            renderEntry.lodTransitionFadeValue;
    }
    if (renderEntry.appendVisibleTile) {
        tilePlan_.visibleTiles.push_back(tile.key);
    }
    if (renderEntry.queueNormalLoad) {
        queueTileLoad(
            tile.key,
            TileLoadPriorityGroup::Normal,
            tilePriority);
    }
}

bool Tileset::hasLodTransitionRenderContent(const TilesetTile& tile) const {
    return tile.contentKind == TileContentKind::Render &&
           TileSelectionRasterOverlayPreparer::isRenderable(
               tile,
               rasterOverlays_);
}

void Tileset::updateLodTransitions(double deltaSeconds) {
    TileLodTransitionController::updateTransitions(
        tilePlan_,
        tilesFadingOut_,
        deltaSeconds,
        TileLodTransitionOptions{
            &tiles_,
            options_.enableLodTransitionPeriod,
            options_.lodTransitionLength},
        [](const TileKey& key) {
            return TileCacheKey::forTile(key);
        },
        [this](const TilesetTile& tile) {
            return hasLodTransitionRenderContent(tile);
        });
}

void Tileset::ensureTileChildren(TilesetTile& tile) {
    TileChildFrameMaterializer::ensureChildren(
        TileChildFrameMaterializeInput{
            tile,
            contentProvider_ ? contentProvider_->childTiles(tile.key)
                             : std::vector<TileKey>{},
            tileScheme_->maxZoom(),
            terrainProvider_ != nullptr,
            isAvailabilityBoundaryTile(tile) &&
                !hasLoadedTerrainContent(tile)},
        [this](const TileKey& key) {
            return ensureTile(key);
        },
        [this](const TileKey& key) {
            return terrainProvider_
                ? terrainProvider_->availabilityState(key)
                : TileAvailabilityState::NotAvailable;
        });
}

void Tileset::createRasterOverlayUpsampledChildren(TilesetTile& tile) {
    const bool changed =
        TileRasterUpsampledChildMaterializer::materialize(
            tile,
            cesiumTerrainGeometricError(tile.bounds),
            [this](const TileKey& key) {
                return ensureTile(key);
            });
    if (changed) {
        markTileResourcesDirty();
    }
}

bool Tileset::canRefine(const TilesetTile& tile) const {
    return TileRefinementAvailabilityResolver::canRefine(
        tile,
        contentProvider_.get(),
        terrainProvider_.get(),
        *tileScheme_,
        terrainCache_,
        [](const TileKey& key) {
            return TileCacheKey::forTile(key);
        },
        [this](const TilesetTile& candidate) {
            return isAvailabilityBoundaryTile(candidate);
        },
        [this](const TilesetTile& candidate) {
            return hasLoadedTerrainContent(candidate);
        });
}

TileTraversalDetails Tileset::visitTileIfNeeded(
    TilesetTile& tile,
    const SelectorFrame& selectorFrame,
    uint32_t depth,
    bool ancestorMeetsSse) {
    const TileSelectionTraversalCounterPlan visitStartCounters =
        TileSelectionTraversalCounterPolicy::planVisitStart();
    selectionCounters_.visited += visitStartCounters.visited;

    const Cartographic cameraCart =
        Ellipsoid::WGS84().cartesianToCartographic(lastCameraPosition_);
    const bool underCamera =
        tile.bounds.contains(cameraCart.longitude(), cameraCart.latitude());
    tile.ancestorMeetsSse = ancestorMeetsSse;
    const TileSelectionVisibilityContext visibilityContext{
        options_.renderTilesUnderCamera,
        cameraCart.longitude(),
        cameraCart.latitude()};

    // cesium-native frustumCull(cullWithChildrenBounds): only
    // replace-refined tiles with finite children use children bounds for
    // tighter culling. ADD content and unconditional descendants can
    // extend outside the child union.
    const TileSelectionVisitPreparationResult preparation =
        TileSelectionVisitPreparation::prepare(
            tile,
            selectorFrame.views,
            selectorFrame.fogDensities,
            visibilityContext,
            TileSelectionVisitPreparationOptions{
                options_.enableFrustumCulling,
                options_.enableFogCulling,
                options_.preloadSiblings,
                options_.forbidHoles,
                options_.enforceCulledScreenSpaceError,
                options_.maximumScreenSpaceError,
                options_.culledScreenSpaceError});
    tile.inFrustum = preparation.visibilitySample.inFrustum;
    tile.cameraInside = underCamera;

    const TileSelectionVisitOutcomePlan visitOutcome =
        TileSelectionVisitPreparation::outcomePlan(preparation);
    const TileSelectionTraversalCounterPlan outcomeCounters =
        TileSelectionTraversalCounterPolicy::planOutcome(visitOutcome);
    selectionCounters_.culled += outcomeCounters.frustumCulled;
    selectionCounters_.fogCulled += outcomeCounters.fogCulled;
    selectionCounters_.culledVisited += outcomeCounters.culledVisited;
    if (visitOutcome.shouldExit) {
        if (visitOutcome.markTileCulled) {
            tile.selectionState = TileSelectionState::Culled;
        }
        if (visitOutcome.resetScreenSpaceError) {
            tile.screenSpaceError = 0.0;
        }
        if (visitOutcome.queueLoad) {
            queueTileLoad(
                tile.key,
                visitOutcome.loadGroup,
                preparation.inputSummary.priority);
        }
        return visitOutcome.returnCulledTraversalDetails
            ? createTraversalDetailsForCulledTile(tile)
            : TileTraversalDetails{};
    }

    return visitTile(tile,
                     selectorFrame,
                     depth,
                     preparation.meetsScreenSpaceError,
                     ancestorMeetsSse,
                     preparation.inputSummary.priority,
                     preparation.inputSummary.screenSpaceError);
}

TileTraversalDetails Tileset::visitTile(
    TilesetTile& tile,
    const SelectorFrame& selectorFrame,
    uint32_t depth,
    bool meetsSse,
    bool ancestorMeetsSse,
    double tilePriority,
    double tileSse) {
    (void)depth;
    TileSelectionRasterOverlayPreparer::prepare(
        tile,
        rasterOverlays_,
        device_,
        options_.maximumScreenSpaceError,
        frameResourceBudget_);
    const bool renderable = TileSelectionRasterOverlayPreparer::isRenderable(
        tile,
        rasterOverlays_);
    tile.renderable = renderable;

    const bool tileCanRefine = canRefine(tile);
    TileSelectionRefineFlowResult refineFlow;
    refineFlow.ancestorMeetsSse = ancestorMeetsSse;
    if (tileCanRefine) {
        const TileSelectionRefineFlowOptions refineFlowOptions{
            options_.enableOcclusionCulling,
            options_.delayRefinementForOcclusion};
        TileSelectionRefineFlowInput refineFlowInput{
            tile.unconditionallyRefine,
            meetsSse,
            ancestorMeetsSse,
            renderable,
            tile.previousSelectionState,
            TileSelectionHistory::childWasRefinedLastFrame(tile),
            std::nullopt};
        refineFlow = TileSelectionRefineFlowPolicy::evaluate(
            refineFlowInput,
            refineFlowOptions);

        // cesium-native: occlusion can stop or delay refinement before descendant
        // traversal, avoiding child loads that may later prove unnecessary.
        if (refineFlow.shouldCheckOcclusion) {
            refineFlowInput.occlusion = checkOcclusion(tile);
            refineFlow = TileSelectionRefineFlowPolicy::evaluate(
                refineFlowInput,
                refineFlowOptions);
            const TileSelectionTraversalCounterPlan occlusionCounters =
                TileSelectionTraversalCounterPolicy::planRefineFlow(
                    refineFlow);
            selectionCounters_.occluded += occlusionCounters.occluded;
            selectionCounters_.waitingForOcclusionResults +=
                occlusionCounters.waitingForOcclusion;
        }
    }

    meetsSse = refineFlow.meetsScreenSpaceError;
    const TileSelectionPreTraversalPlan preTraversal =
        TileSelectionPreTraversalPolicy::plan(
            TileSelectionPreTraversalInput{
                tileCanRefine,
                tile.refine,
                refineFlow});
    ancestorMeetsSse = preTraversal.ancestorMeetsSseAfterPreTraversal;
    bool queuedForLoad = preTraversal.queuedForLoadAfterPreTraversal;
    if (preTraversal.queueUrgentLoad) {
        queueTileLoad(
            tile.key,
            TileLoadPriorityGroup::Urgent,
            tilePriority);
    }

    if (preTraversal.finishAsSingleTile) {
        addTileToCurrentPlan(
            tile,
            tileSse,
            preTraversal.singleTileShouldQueueLoad,
            tilePriority);
        return createTraversalDetailsForSingleTile(tile);
    }

    ensureTileChildren(tile);

    if (preTraversal.addAdditiveParentToPlan) {
        addTileToCurrentPlan(
            tile,
            tileSse,
            preTraversal.additiveParentShouldQueueLoad,
            tilePriority);
    }

    const size_t firstRenderedDescendant = tilePlan_.visibleTiles.size();
    const size_t loadQueueBeforeChildren = loadQueue_.size();

    const TileTraversalDetails traversalDetails =
        TileSelectionChildTraversal::visitChildren(
            tile.children,
            [this, &selectorFrame, depth, ancestorMeetsSse](
                TilesetTile& child) {
                return visitTileIfNeeded(
                    child,
                    selectorFrame,
                    depth + 1,
                    ancestorMeetsSse);
            });

    const TileSelectionPostTraversalResult postTraversal =
        TileSelectionPostTraversalPolicy::evaluate(
            TileSelectionPostTraversalInput{
                traversalDetails,
                renderable,
                tile.unconditionallyRefine,
                tile.previousSelectionState,
                hasLodTransitionRenderContent(tile),
                tile.lodTransitionFadePercentage,
                TileSelectionHistory::wasRenderedLastFrame(tile),
                tile.contentKind == TileContentKind::External,
                tile.refine,
                queuedForLoad},
            TileSelectionPostTraversalOptions{
                options_.loadingDescendantLimit,
                options_.enableLodTransitionPeriod,
                options_.kickDescendantsWhileFadingIn,
                options_.preloadAncestors});

    const TileSelectionPostTraversalCommitPlan postCommit =
        TileSelectionPostTraversalPolicy::commitPlan(
            postTraversal,
            queuedForLoad);

    const TileSelectionPostTraversalCommitResult commitResult =
        TileSelectionPostTraversalCommitter::commit(
            tile,
            tilePlan_,
            loadQueue_,
            selectionCounters_,
            postCommit,
            TileSelectionPostTraversalCommitContext{
                firstRenderedDescendant,
                loadQueueBeforeChildren,
                tileSse,
                tilePriority,
                renderable},
            [](TilesetTile& kickedTile) {
                kickVisitedDescendants(kickedTile);
            },
            [this](const TileKey& key,
                   TileLoadPriorityGroup group,
                   double priority) {
                queueTileLoad(key, group, priority);
            },
            [this](TilesetTile& selectedTile,
                   double screenSpaceError,
                   bool queueForLoad,
                   double priority) {
                addTileToCurrentPlan(
                    selectedTile,
                    screenSpaceError,
                    queueForLoad,
                    priority);
            });

    if (commitResult.returnedSingleTileDetails) {
        return commitResult.details;
    }
    return traversalDetails;
}

void Tileset::refreshTilePlanRenderEntries() {
    TileRenderPlanFinalizer::refreshRenderEntries(
        tilePlan_,
        TileRenderPlanFinalizeOptions{
            options_.enableLodTransitionPeriod,
            interactionActiveForFrame_,
            resourceSmoothingActiveForFrame_,
            kActiveInteractionRenderPrepBudget,
            kRecoveryRenderPrepBudget},
            [this](const TileKey& key) {
                return ensureTile(key);
            },
            [this](const TilesetTile& tile) {
                return hasSurfaceDrawable(tile);
            },
            [](const TileKey& key) {
                return TileCacheKey::forTile(key);
            });
}

Tileset::TilePlanFinalizeTimings
Tileset::finalizeSelectedTilePlan(const FrameState& frameState) {
    const TileSelectionFrameFinalizeTimings timings =
        TileSelectionFrameFinalizer::finalize(
            tilePlan_,
            tiles_,
            selectionCounters_,
            frameState.deltaSeconds,
            [this](double deltaSeconds) {
                updateLodTransitions(deltaSeconds);
            },
            [this]() {
                refreshTilePlanRenderEntries();
            },
            [this](const TilesetTile& tile) {
                return TileSelectionRasterOverlayPreparer::isRenderable(
                    tile,
                    rasterOverlays_);
            });
    return TilePlanFinalizeTimings{
        timings.dedupeMs,
        timings.transitionMs,
        timings.summaryMs};
}

void Tileset::selectTiles(const FrameState& frameState) {
    currentFrameTimeSeconds_ = frameState.timeSeconds;
    TileSelectionFrameRunner::run(
        TileSelectionFrameRunInput{
            tilePlan_,
            loadQueue_,
            selectionCounters_,
            frameState,
            options_.fogDensityTable,
            tileScheme_->id(),
            contentProvider_ ? contentProvider_->rootTiles()
                             : std::vector<TileKey>{}},
        [this]() {
            resetTileSelectionState();
        },
        [this](const TileKey& key) {
            return ensureTile(key);
        },
        [this](TilesetTile& root, const SelectorFrame& selectorFrame) {
            visitTileIfNeeded(root, selectorFrame, 0, false);
        },
        [this](const FrameState& finalizeFrameState) {
            return finalizeSelectedTilePlan(finalizeFrameState);
        });
}

void Tileset::update(const FrameState& frameState) {
    if (!frameState.camera) return;
    const double updateStartMs = perf::nowMs();

    // cesium-native: increment generation each frame so that
    // RenderCommand validator (non-zero check) accepts SurfaceTile commands.
    ++generation_;

    const TileUpdateFrameContext frameContext =
        TileUpdateFrameContextBuilder::build(
            frameState,
            lastCameraPosition_,
            lastInteractionActiveTimeSeconds_,
            TileUpdateFrameContextOptions{
                options_.maximumSimultaneousTileLoads,
                options_.mainThreadLoadingTimeLimit,
                kPostInteractionResourceSmoothingSeconds});
    const TileFrameInteractionSnapshot& interactionSnapshot =
        frameContext.interaction;
    cameraMoving_ = interactionSnapshot.cameraMoving;
    lastCameraPosition_ = interactionSnapshot.cameraPosition;
    lastCameraDirection_ = interactionSnapshot.cameraDirection;
    const bool interactionActive = interactionSnapshot.interactionActive;
    interactionActiveForFrame_ = interactionActive;
    lastInteractionActiveTimeSeconds_ =
        interactionSnapshot.lastInteractionActiveTimeSeconds;
    resourceSmoothingActiveForFrame_ =
        interactionSnapshot.resourceSmoothingActive;

    frameResourceBudget_.beginFrame(
        frameState.frameId,
        frameContext.resourceBudgetConfig);

    const TileUpdateUploadRunResult uploadWork =
        TileUpdateUploadRunner::run(
            TileUpdateUploadRunInput{
                rasterOverlays_,
                frameResourceBudget_,
                interactionActive,
                resourceSmoothingActiveForFrame_},
            [this](bool uploadInteractionActive,
                   bool uploadResourceSmoothingActive,
                   FrameResourceBudget* budget) {
                return processPendingUploads(
                    uploadInteractionActive,
                    uploadResourceSmoothingActive,
                    budget);
            },
            [this]() {
                markTileResourcesDirty();
            });

    const uint64_t currentResourceRevision =
        TileRasterOverlaySignature::selectionResourceRevision(
            resourceRevision_,
            rasterOverlays_);
    const uint64_t currentOverlaySignature =
        TileRasterOverlaySignature::configuration(rasterOverlays_);
    const bool reusedSelection = selectionReuseState_.canReuse(
        frameState,
        currentResourceRevision,
        currentOverlaySignature,
        !tilePlan_.tilesFadingOut.empty() || tilePlan_.fadingNodeCount > 0,
        hasTilesetPendingWork(),
        TileRasterOverlaySignature::hasPendingWork(rasterOverlays_));

    const TileUpdateSelectionWorkResult selectionWork =
        TileUpdateSelectionWorkRunner::run(
            TileUpdateSelectionWorkInput{
                tilePlan_,
                loadQueue_,
                selectionCounters_,
                selectionReuseState_,
                rasterOverlays_,
                frameResourceBudget_,
                device_,
                frameState,
                currentResourceRevision,
                currentOverlaySignature,
                reusedSelection,
                options_.maximumScreenSpaceError},
            [this]() {
                refreshTilePlanRenderEntries();
            },
            [this](const FrameState& selectionFrameState) {
                selectTiles(selectionFrameState);
            },
            [this](const TileKey& key) {
                return ensureTile(key);
            },
            [this](const std::vector<TileLoadRequest>& requests,
                   FrameResourceBudget* budget) {
                return requestMissingTiles(requests, budget);
            });

    const std::array<char, 384> updateDetail =
        TileFrameDebugLogFormatter::updateDetail(
            TileUpdateDebugLogInput{
                tilePlan_.visibleTiles.size(),
                loadQueue_.size(),
                selectionWork.computeMs,
                selectionWork.prefetchMs,
                selectionWork.requestMs,
                uploadWork.terrainUploadMs,
                uploadWork.rasterUploadMs,
                terrainCache_.size(),
                loadLifecycle_.requestState().totalRequestCount(),
                selectionCounters_,
                selectionWork.reusedSelection,
                uploadWork.rasterUploadsProcessed,
                interactionActive,
                resourceSmoothingActiveForFrame_});
    perf::logTimingAtLeast(frameState.frameId,
                           "Tileset.update",
                           perf::nowMs() - updateStartMs,
                           10.0,
                           updateDetail.data());
}

TileLoadRequestOutcome Tileset::requestMissingTiles(
    const std::vector<TileLoadRequest>& loadRequests,
    FrameResourceBudget* budget) {
    FrameResourceBudget localBudget;
    if (!budget) {
        const FrameResourceBudgetConfig config =
            TileFrameBudgetFallback::requestConfig(
                options_.maximumSimultaneousTileLoads,
                options_.mainThreadLoadingTimeLimit);
        localBudget.beginFrame(frameNumber_, config);
        budget = &localBudget;
    }
    return TileMissingRequestScheduler::request(
        loadRequests,
        TileMissingRequestSchedulerInput{
            loadLifecycle_,
            *budget,
            terrainProvider_.get(),
            contentProvider_.get(),
            tiles_,
            terrainCache_,
            emptyContentRegistry_},
        [](const TileKey& key) {
            return TileCacheKey::forTile(key);
        },
        [this](TilesetTile& tile, double priority) {
            return prepareUpsampleSourceTile(tile, priority);
        },
        [this](const TileKey& key) {
            return ensureTile(key);
        });
}

bool Tileset::processPendingUploads(bool interactionActive,
                                    bool resourceSmoothingActive,
                                    FrameResourceBudget* budget) {
    FrameResourceBudget localBudget;
    if (!budget) {
        const FrameResourceBudgetConfig config =
            TileFrameBudgetFallback::uploadConfig(
                options_.maximumSimultaneousTileLoads,
                options_.mainThreadLoadingTimeLimit,
                interactionActive,
                resourceSmoothingActive,
                static_cast<uint32_t>(kSmoothedMainThreadUploadLimit));
        localBudget.beginFrame(frameNumber_, config);
        budget = &localBudget;
    }
    return TilePendingUploadFrameProcessor::process(
        TilePendingUploadFrameProcessorInput{
            loadLifecycle_,
            *budget,
            terrainProvider_.get(),
            terrainCache_,
            emptyContentRegistry_,
            interactionActive,
            resourceSmoothingActive},
        [this](const TileKey& key) {
            return ensureTile(key);
        },
        [this](TilesetTile& tile) {
            ensureTileChildren(tile);
        },
        [this](TilesetTile& tile) {
            ensureTileMesh(tile);
        },
        [this](TilesetTile& tile) {
            GltfRenderResourcePreparer::prepare(
                tile,
                device_,
                currentFrameTimeSeconds_);
        },
        [this](const TileKey& key, DecodedHeightmap& heightmap) {
            TileQuantizedMeshAvailabilityIngestor::ingest(
                terrainProvider_.get(),
                key,
                heightmap);
        },
        [this]() {
            markTileResourcesDirty();
        });
}

void Tileset::ensureTileMesh(TilesetTile& tile) {
    TileMeshFrameEnsurer::ensure(
        TileMeshFrameEnsureInput{
            tile,
            terrainCache_,
            device_,
            terrainProvider_ != nullptr},
        [](const TileKey& key) {
            return TileCacheKey::forTile(key);
        },
        [this](const TileKey& key, DecodedHeightmap& heightmap) {
            TileQuantizedMeshAvailabilityIngestor::ingest(
                terrainProvider_.get(),
                key,
                heightmap);
        },
        [](const TilesetTile& sourceTile, bool allowUnloadingSource) {
            return TileUpsampleSourcePreparer::findSourceTile(
                sourceTile,
                allowUnloadingSource);
        },
        [this](TilesetTile& ancestor) {
            ensureTileMesh(ancestor);
        },
        [this](const TilesetTile& drawableTile) {
            return hasSurfaceDrawable(drawableTile);
        },
        [this](const TilesetTile& renderableTile) {
            return TileSelectionRasterOverlayPreparer::isCompleteRenderable(
                renderableTile,
                rasterOverlays_);
        },
        [this]() {
            markTileResourcesDirty();
            // Bytes are recomputed before unload; overlay textures may attach
            // or detach later, and recompute captures the current state.
        });
}

void Tileset::buildTileDrawCommand(
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
            options_.maximumScreenSpaceError,
            transitionOpacity,
            allowSynchronousMeshPrep,
            surfaceClipUv},
        [this](TilesetTile& meshTile) {
            ensureTileMesh(meshTile);
        },
        [this](const TilesetTile& drawableTile) {
            return hasSurfaceDrawable(drawableTile);
        },
        [this, &renderer](TilesetTile& unloadTile) {
            unloadTileContent(unloadTile, &renderer);
        },
        [this](TilesetTile& upsampleTile) {
            createRasterOverlayUpsampledChildren(upsampleTile);
        });
}

void Tileset::buildRenderCommands(Renderer& renderer,
                                   RenderCommandList& commands) {
    ++frameNumber_;
    TileRenderFrameBuilder::build(
        TileRenderFrameInputBuilder::build(
            tilePlan_,
            tiles_,
            unloadQueue_,
            rasterOverlays_,
            cacheBytesDirty_,
            frameNumber_,
            lastCameraPosition_,
            options_.fogDensityTable,
            selectionCounters_.fogCulled,
            resourceSmoothingActiveForFrame_,
            interactionActiveForFrame_,
            totalBytesUsed_,
            options_.maximumCachedBytes),
        renderer,
        commands,
        [this](const TileKey& key) {
            return ensureTile(key);
        },
        [](const TileKey& key) {
            return TileCacheKey::forTile(key);
        },
        [this](const std::string& cacheKey) {
            markIneligibleForUnloading(cacheKey);
        },
        [this](Renderer& renderer,
               TilesetTile& tile,
               RenderCommandList& commands,
               float transitionOpacity,
               bool allowSynchronousMeshPrep,
               const std::optional<std::array<float, 4>>& surfaceClipUv) {
            buildTileDrawCommand(
                renderer,
                tile,
                commands,
                transitionOpacity,
                allowSynchronousMeshPrep,
                surfaceClipUv);
        },
        [&renderer](
            const std::vector<TileFrameInactiveEntry>& inactiveTiles) {
            TileSubtreeRemovalCoordinator::detachInactiveRasterOverlays(
                inactiveTiles,
                &renderer);
        },
        [this](const std::string& cacheKey) {
            markEligibleForUnloading(cacheKey);
        },
        [this]() {
            updateTotalBytesUsed();
        },
        [this, &renderer]() {
            unloadCachedBytes(options_.maximumCachedBytes, &renderer);
        });
}

void Tileset::releaseRenderReferences() {
    // cesium-native: called from Scene after renderer_->submit().
    // Drops the reference that was added in buildRenderCommands for
    // tiles in the current render list, making them eligible for
    // unloading in the next frame.
    TileRenderReferenceReleaser::release(tiles_);
}

} // namespace earth_engine
