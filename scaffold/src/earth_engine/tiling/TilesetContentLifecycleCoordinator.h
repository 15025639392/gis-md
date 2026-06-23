#pragma once

#include "GltfRenderResourcePreparer.h"
#include "LegacyHeightmapTerrainCacheMode.h"
#include "TileCacheKey.h"
#include "TileEmptyContentRegistry.h"
#include "TileFrameBudgetFallback.h"
#include "TileLoadLifecycle.h"
#include "TileLoadTypes.h"
#include "TileMissingRequestScheduler.h"
#include "TilePendingUploadFrameProcessor.h"

#include "../content/GltfContentProvider.h"
#include "../core/resources/FrameResourceBudget.h"
#include "../providers/TerrainProvider.h"
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
    TerrainProvider* legacyTerrainProvider = nullptr;
    TilesetContentProvider* contentProvider = nullptr;
    RenderDevice* device = nullptr;
    const std::vector<ActivatedRasterOverlay*>& rasterOverlays;
    const std::unordered_map<std::string, std::unique_ptr<TilesetTile>>& tiles;
    std::unordered_map<std::string, std::unique_ptr<DecodedHeightmap>>&
        terrainCache;
    LegacyHeightmapTerrainCacheMode legacyHeightmapCacheMode =
        LegacyHeightmapTerrainCacheMode::Include;
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
    std::unordered_map<std::string, std::unique_ptr<DecodedHeightmap>>&
        terrainCache;
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
                context.legacyTerrainProvider,
                context.contentProvider,
                context.tiles,
                context.terrainCache,
                context.legacyHeightmapCacheMode,
                context.emptyContentRegistry},
            [](const TileKey& key) {
                return TileCacheKey::forTile(key);
            },
            prepareUpsampleSourceTile,
            ensureTile);
    }

    template <typename EnsureTileFn,
              typename EnsureTileChildrenFn,
              typename EnsureTileMeshFn,
              typename MarkResourcesDirtyFn>
    static bool processPendingUploads(
        TilesetContentUploadContext context,
        bool interactionActive,
        bool resourceSmoothingActive,
        FrameResourceBudget* budget,
        EnsureTileFn&& ensureTile,
        EnsureTileChildrenFn&& ensureTileChildren,
        EnsureTileMeshFn&& ensureTileMesh,
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
                context.terrainCache,
                context.emptyContentRegistry,
                interactionActive,
                resourceSmoothingActive},
            ensureTile,
            ensureTileChildren,
            ensureTileMesh,
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
