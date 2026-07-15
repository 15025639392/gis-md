#pragma once

#include "TileEmptyContentRegistry.h"
#include "TileLoadLifecycle.h"
#include "TileLoadTypes.h"
#include "TilesetContentLifecycleCoordinator.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace earth_engine {

class ActivatedRasterOverlay;
class IPrepareRendererResources;

class TileContentLifecycleManager {
public:
    TileContentLifecycleManager() = default;
    ~TileContentLifecycleManager();

    TileLoadLifecycle& loadLifecycle() { return loadLifecycle_; }
    const TileLoadLifecycle& loadLifecycle() const { return loadLifecycle_; }

    TileEmptyContentRegistry& emptyContentRegistry() {
        return emptyContentRegistry_;
    }
    const TileEmptyContentRegistry& emptyContentRegistry() const {
        return emptyContentRegistry_;
    }

    int pendingRequests() const;
    bool hasPendingWork() const;
    void shutdown();

    template <typename EnsureTileFn, typename MarkResourcesDirtyFn>
    bool drainGpuUploadQueue(
        TilesetContentProvider* contentProvider,
        RenderDevice* device,
        IPrepareRendererResources* pPrepRenderer,
        const std::vector<ActivatedRasterOverlay*>& rasterOverlays,
        GpuUploadQueue* gpuUploadQueue,
        uint64_t frameNumber,
        uint32_t maximumSimultaneousTileLoads,
        double mainThreadLoadingTimeLimit,
        double currentFrameTimeSeconds,
        uint32_t smoothedMainThreadUploadLimit,
        FrameResourceBudget* budget,
        uint32_t maxUploadsPerFrame,
        EnsureTileFn&& ensureTile,
        MarkResourcesDirtyFn&& markResourcesDirty) {
        return TilesetContentLifecycleCoordinator::drainGpuUploadQueue(
            makeUploadContext(
                contentProvider,
                device,
                pPrepRenderer,
                rasterOverlays,
                gpuUploadQueue,
                frameNumber,
                maximumSimultaneousTileLoads,
                mainThreadLoadingTimeLimit,
                currentFrameTimeSeconds,
                smoothedMainThreadUploadLimit),
            budget,
            maxUploadsPerFrame,
            std::forward<EnsureTileFn>(ensureTile),
            std::forward<MarkResourcesDirtyFn>(markResourcesDirty));
    }

    template <typename LoadRequests,
              typename PrepareUpsampleSourceTileFn,
              typename EnsureTileFn>
    TileLoadRequestOutcome requestMissingTiles(
        LoadRequests& loadRequests,
        TilesetContentProvider* contentProvider,
        RenderDevice* device,
        const std::vector<ActivatedRasterOverlay*>& rasterOverlays,
        const std::unordered_map<std::string, std::unique_ptr<TilesetTile>>&
            tiles,
        uint64_t frameNumber,
        uint32_t maximumSimultaneousTileLoads,
        double mainThreadLoadingTimeLimit,
        double currentFrameTimeSeconds,
        uint32_t smoothedMainThreadUploadLimit,
        FrameResourceBudget* budget,
        PrepareUpsampleSourceTileFn&& prepareUpsampleSourceTile,
        EnsureTileFn&& ensureTile) {
        return TilesetContentLifecycleCoordinator::requestMissingTiles(
            loadRequests,
            makeContext(
                contentProvider,
                device,
                rasterOverlays,
                tiles,
                frameNumber,
                maximumSimultaneousTileLoads,
                mainThreadLoadingTimeLimit,
                currentFrameTimeSeconds,
                smoothedMainThreadUploadLimit),
            budget,
            std::forward<PrepareUpsampleSourceTileFn>(
                prepareUpsampleSourceTile),
            std::forward<EnsureTileFn>(ensureTile));
    }

    template <typename EnsureTileFn,
              typename EnsureTileChildrenFn,
              typename MarkResourcesDirtyFn>
    bool processPendingUploads(
        TilesetContentProvider* contentProvider,
        RenderDevice* device,
        IPrepareRendererResources* pPrepRenderer,
        const std::vector<ActivatedRasterOverlay*>& rasterOverlays,
        GpuUploadQueue* gpuUploadQueue,
        uint64_t frameNumber,
        uint32_t maximumSimultaneousTileLoads,
        double mainThreadLoadingTimeLimit,
        double currentFrameTimeSeconds,
        uint32_t smoothedMainThreadUploadLimit,
        bool interactionActive,
        bool resourceSmoothingActive,
        FrameResourceBudget* budget,
        EnsureTileFn&& ensureTile,
        EnsureTileChildrenFn&& ensureTileChildren,
        MarkResourcesDirtyFn&& markResourcesDirty) {
        return TilesetContentLifecycleCoordinator::processPendingUploads(
            makeUploadContext(
                contentProvider,
                device,
                pPrepRenderer,
                rasterOverlays,
                gpuUploadQueue,
                frameNumber,
                maximumSimultaneousTileLoads,
                mainThreadLoadingTimeLimit,
                currentFrameTimeSeconds,
                smoothedMainThreadUploadLimit),
            interactionActive,
            resourceSmoothingActive,
            budget,
            std::forward<EnsureTileFn>(ensureTile),
            std::forward<EnsureTileChildrenFn>(ensureTileChildren),
            std::forward<MarkResourcesDirtyFn>(markResourcesDirty));
    }

private:
    TilesetContentLifecycleContext makeContext(
        TilesetContentProvider* contentProvider,
        RenderDevice* device,
        const std::vector<ActivatedRasterOverlay*>& rasterOverlays,
        const std::unordered_map<std::string, std::unique_ptr<TilesetTile>>&
            tiles,
        uint64_t frameNumber,
        uint32_t maximumSimultaneousTileLoads,
        double mainThreadLoadingTimeLimit,
        double currentFrameTimeSeconds,
        uint32_t smoothedMainThreadUploadLimit) {
        return TilesetContentLifecycleContext{
            loadLifecycle_,
            contentProvider,
            device,
            rasterOverlays,
            tiles,
            emptyContentRegistry_,
            frameNumber,
            maximumSimultaneousTileLoads,
            mainThreadLoadingTimeLimit,
            currentFrameTimeSeconds,
            smoothedMainThreadUploadLimit};
    }

    TilesetContentUploadContext makeUploadContext(
        TilesetContentProvider* contentProvider,
        RenderDevice* device,
        IPrepareRendererResources* pPrepRenderer,
        const std::vector<ActivatedRasterOverlay*>& rasterOverlays,
        GpuUploadQueue* gpuUploadQueue,
        uint64_t frameNumber,
        uint32_t maximumSimultaneousTileLoads,
        double mainThreadLoadingTimeLimit,
        double currentFrameTimeSeconds,
        uint32_t smoothedMainThreadUploadLimit) {
        return TilesetContentUploadContext{
            loadLifecycle_,
            contentProvider,
            device,
            pPrepRenderer,
            rasterOverlays,
            emptyContentRegistry_,
            gpuUploadQueue,
            frameNumber,
            maximumSimultaneousTileLoads,
            mainThreadLoadingTimeLimit,
            currentFrameTimeSeconds,
            smoothedMainThreadUploadLimit};
    }

    TileLoadLifecycle loadLifecycle_;
    TileEmptyContentRegistry emptyContentRegistry_;
};

} // namespace earth_engine
