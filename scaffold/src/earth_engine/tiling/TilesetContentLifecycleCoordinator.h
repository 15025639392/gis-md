#pragma once

#include "GltfRenderResourcePreparer.h"
#include "TileCacheKey.h"
#include "TileEmptyContentRegistry.h"
#include "TileFrameBudgetFallback.h"
#include "TileLoadLifecycle.h"
#include "TileLoadTypes.h"
#include "TileMissingRequestScheduler.h"
#include "TilePendingUploadFrameProcessor.h"

#include "../content/GltfContentProvider.h"
#include "../core/resources/FrameResourceBudget.h"
#include "../renderer/RenderDevice.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace earth_engine {

class ActivatedRasterOverlay;
class IPrepareRendererResources;

struct TilesetContentLifecycleContext {
    TileLoadLifecycle& loadLifecycle;
    TilesetContentProvider* contentProvider = nullptr;
    RenderDevice* device = nullptr;
    const std::vector<ActivatedRasterOverlay*>& rasterOverlays;
    const std::unordered_map<std::string, std::unique_ptr<TilesetTile>>& tiles;
    TileEmptyContentRegistry& emptyContentRegistry;
    uint64_t frameNumber = 0;
    uint32_t maximumSimultaneousTileLoads = 20;
    double mainThreadLoadingTimeLimit = 0.0;
    double currentFrameTimeSeconds = 0.0;
    uint32_t smoothedMainThreadUploadLimit = 1;
};

struct TilesetContentUploadContext {
    TileLoadLifecycle& loadLifecycle;
    TilesetContentProvider* contentProvider = nullptr;
    RenderDevice* device = nullptr;
    IPrepareRendererResources* pPrepRenderer = nullptr;
    const std::vector<ActivatedRasterOverlay*>& rasterOverlays;
    TileEmptyContentRegistry& emptyContentRegistry;
    uint64_t frameNumber = 0;
    uint32_t maximumSimultaneousTileLoads = 20;
    double mainThreadLoadingTimeLimit = 0.0;
    double currentFrameTimeSeconds = 0.0;
    uint32_t smoothedMainThreadUploadLimit = 1;
};

class TilesetContentLifecycleCoordinator {
public:
    template <typename PrepareUpsampleSourceTileFn, typename EnsureTileFn>
    static TileLoadRequestOutcome requestMissingTiles(
        const std::vector<TileLoadRequest>& loadRequests,
        TilesetContentLifecycleContext context,
        FrameResourceBudget* budget,
        PrepareUpsampleSourceTileFn&& prepareUpsampleSourceTile,
        EnsureTileFn&& ensureTile) {
        FrameResourceBudget localBudget;
        if (!budget) {
            const FrameResourceBudgetConfig config =
                TileFrameBudgetFallback::requestConfig(
                    context.maximumSimultaneousTileLoads,
                    context.mainThreadLoadingTimeLimit);
            localBudget.beginFrame(context.frameNumber, config);
            budget = &localBudget;
        }
        return TileMissingRequestScheduler::request(
            loadRequests,
            TileMissingRequestSchedulerInput{
                context.loadLifecycle,
                *budget,
                context.contentProvider,
                context.tiles,
                context.emptyContentRegistry},
            [](const TileKey& key) {
                return TileCacheKey::forTile(key);
            },
            prepareUpsampleSourceTile,
            ensureTile);
    }

    template <typename EnsureTileFn,
              typename EnsureTileChildrenFn,
              typename MarkResourcesDirtyFn>
    static bool processPendingUploads(
        TilesetContentUploadContext context,
        bool interactionActive,
        bool resourceSmoothingActive,
        FrameResourceBudget* budget,
        EnsureTileFn&& ensureTile,
        EnsureTileChildrenFn&& ensureTileChildren,
        MarkResourcesDirtyFn&& markResourcesDirty) {
        FrameResourceBudget localBudget;
        if (!budget) {
            const FrameResourceBudgetConfig config =
                TileFrameBudgetFallback::uploadConfig(
                    context.maximumSimultaneousTileLoads,
                    context.mainThreadLoadingTimeLimit,
                    interactionActive,
                    resourceSmoothingActive,
                    context.smoothedMainThreadUploadLimit);
            localBudget.beginFrame(context.frameNumber, config);
            budget = &localBudget;
        }
        return TilePendingUploadFrameProcessor::process(
            TilePendingUploadFrameProcessorInput{
                context.loadLifecycle,
                *budget,
                context.contentProvider,
                context.device,
                context.pPrepRenderer,
                context.rasterOverlays,
                context.emptyContentRegistry,
                interactionActive,
            resourceSmoothingActive},
            ensureTile,
            ensureTileChildren,
            [&](TilesetTile& tile) {
                GltfRenderResourcePreparer::prepare(
                    tile,
                    context.device,
                    context.currentFrameTimeSeconds);
            },
            markResourcesDirty);
    }
};

} // namespace earth_engine
