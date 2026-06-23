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

using HeightmapTerrainCache =
    std::unordered_map<std::string, std::unique_ptr<DecodedHeightmap>>;

class TileContentLifecycleManager {
public:
    TileContentLifecycleManager() = default;
    ~TileContentLifecycleManager();

    TileLoadLifecycle& loadLifecycle() { return loadLifecycle_; }
    const TileLoadLifecycle& loadLifecycle() const { return loadLifecycle_; }

    HeightmapTerrainCache& heightmapTerrainCache() {
        return heightmapTerrainCache_;
    }
    const HeightmapTerrainCache& heightmapTerrainCache() const {
        return heightmapTerrainCache_;
    }

    TileEmptyContentRegistry& emptyContentRegistry() {
        return emptyContentRegistry_;
    }
    const TileEmptyContentRegistry& emptyContentRegistry() const {
        return emptyContentRegistry_;
    }

    int pendingRequests() const;
    bool hasPendingWork() const;
    void shutdown();

    template <typename PrepareUpsampleSourceTileFn, typename EnsureTileFn>
    TileLoadRequestOutcome requestMissingTiles(
        const std::vector<TileLoadRequest>& loadRequests,
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
        if (contentProviderOwnsTerrainQuadtree(contentProvider)) {
            heightmapTerrainCache_.clear();
        }
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
    static bool contentProviderOwnsTerrainQuadtree(
        const TilesetContentProvider* contentProvider) {
        return contentProvider && contentProvider->providesTerrainQuadtree();
    }

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
            heightmapTerrainCache_,
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
            frameNumber,
            maximumSimultaneousTileLoads,
            mainThreadLoadingTimeLimit,
            currentFrameTimeSeconds,
            smoothedMainThreadUploadLimit};
    }

    TileLoadLifecycle loadLifecycle_;
    std::unordered_map<std::string, std::unique_ptr<DecodedHeightmap>>
        heightmapTerrainCache_;
    TileEmptyContentRegistry emptyContentRegistry_;
};

} // namespace earth_engine
