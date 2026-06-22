#pragma once

#include "TileEmptyContentRegistry.h"
#include "TileLoadLifecycle.h"
#include "TilePendingLoadCommitCoordinator.h"
#include "TilePendingLoadProcessor.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace earth_engine {

class TilesetContentProvider;
class ActivatedRasterOverlay;
class IPrepareRendererResources;
class RenderDevice;
struct DecodedHeightmap;
struct TileKey;
struct TilesetTile;

struct TilePendingUploadFrameProcessorInput {
    TileLoadLifecycle& loadLifecycle;
    FrameResourceBudget& budget;
    TilesetContentProvider* contentProvider = nullptr;
    RenderDevice* device = nullptr;
    IPrepareRendererResources* pPrepRenderer = nullptr;
    const std::vector<ActivatedRasterOverlay*>& rasterOverlays;
    std::unordered_map<std::string, std::unique_ptr<DecodedHeightmap>>&
        terrainCache;
    TileEmptyContentRegistry& emptyContentRegistry;
    bool interactionActive = false;
    bool resourceSmoothingActive = false;
};

class TilePendingUploadFrameProcessor {
public:
    template <typename EnsureTileFn,
              typename EnsureTileChildrenFn,
              typename EnsureTileMeshFn,
              typename EnsureGltfResourcesFn,
              typename MarkResourcesDirtyFn>
    static bool process(
        TilePendingUploadFrameProcessorInput input,
        EnsureTileFn&& ensureTile,
        EnsureTileChildrenFn&& ensureTileChildren,
        EnsureTileMeshFn&& ensureTileMesh,
        EnsureGltfResourcesFn&& ensureGltfResources,
        MarkResourcesDirtyFn&& markResourcesDirty) {
        auto processTerminalResult = [&](PendingTileLoad& result) {
            TilePendingLoadCommitCoordinator::commitTerminalResult(
                result,
                input.emptyContentRegistry,
                input.pPrepRenderer,
                ensureTile,
                ensureTileChildren,
                markResourcesDirty);
        };

        auto processUpload = [&](PendingTileLoad& upload) {
            TilePendingLoadCommitCoordinator::commitUpload(
                upload,
                input.contentProvider,
                input.device,
                input.pPrepRenderer,
                input.rasterOverlays,
                input.terrainCache,
                input.loadLifecycle,
                input.resourceSmoothingActive,
                ensureTile,
                ensureTileMesh,
                ensureGltfResources,
                markResourcesDirty);
        };

        return TilePendingLoadProcessor::processPendingLoads(
            TilePendingLoadProcessorInput{
                input.loadLifecycle,
                input.budget,
                input.interactionActive,
                {}},
            processTerminalResult,
            processUpload);
    }
};

} // namespace earth_engine
