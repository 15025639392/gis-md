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

    std::unordered_map<std::string, std::unique_ptr<DecodedHeightmap>>&
    legacyTerrainCache() {
        return legacyTerrainCache_;
    }
    const std::unordered_map<std::string, std::unique_ptr<DecodedHeightmap>>&
    legacyTerrainCache() const {
        return legacyTerrainCache_;
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
    void discardLegacyTerrainCache(bool discard);

    template <typename PrepareUpsampleSourceTileFn, typename EnsureTileFn>
    TileLoadRequestOutcome requestMissingTiles(
        const std::vector<TileLoadRequest>& loadRequests,
        TerrainProvider* legacyTerrainProvider,
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
        normalizeContentOwnedTerrainInputs(
            contentProvider,
            legacyTerrainProvider);
        return TilesetContentLifecycleCoordinator::requestMissingTiles(
            loadRequests,
            makeContext(
                legacyTerrainProvider,
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

    void normalizeContentOwnedTerrainInputs(
        const TilesetContentProvider* contentProvider,
        TerrainProvider*& legacyTerrainProvider) {
        if (contentProviderOwnsTerrainQuadtree(contentProvider)) {
            legacyTerrainProvider = nullptr;
            legacyTerrainCache_.clear();
        }
    }

    TilesetContentLifecycleContext makeContext(
        TerrainProvider* legacyTerrainProvider,
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
            legacyTerrainProvider,
            contentProvider,
            device,
            rasterOverlays,
            tiles,
            legacyTerrainCache_,
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
        legacyTerrainCache_;
    TileEmptyContentRegistry emptyContentRegistry_;
};

} // namespace earth_engine
