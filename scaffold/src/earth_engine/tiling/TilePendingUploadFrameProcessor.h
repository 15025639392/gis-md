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

class TerrainProvider;
class ActivatedRasterOverlay;
class RenderDevice;
struct DecodedHeightmap;
struct TileKey;
struct TilesetTile;

struct TilePendingUploadFrameProcessorInput {
    TileLoadLifecycle& loadLifecycle;
    FrameResourceBudget& budget;
    TerrainProvider* terrainProvider = nullptr;
    RenderDevice* device = nullptr;
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
              typename IngestAvailabilityFn,
              typename MarkResourcesDirtyFn>
    static bool process(
        TilePendingUploadFrameProcessorInput input,
        EnsureTileFn&& ensureTile,
        EnsureTileChildrenFn&& ensureTileChildren,
        EnsureTileMeshFn&& ensureTileMesh,
        EnsureGltfResourcesFn&& ensureGltfResources,
        IngestAvailabilityFn&& ingestAvailability,
        MarkResourcesDirtyFn&& markResourcesDirty) {
        auto processTerminalResult = [&](PendingTileLoad& result) {
            TilePendingLoadCommitCoordinator::commitTerminalResult(
                result,
                input.emptyContentRegistry,
                ensureTile,
                ensureTileChildren,
                markResourcesDirty);
        };

        auto processUpload = [&](PendingTileLoad& upload) {
            TilePendingLoadCommitCoordinator::commitUpload(
                upload,
                input.terrainProvider,
                input.device,
                input.rasterOverlays,
                input.terrainCache,
                input.loadLifecycle,
                input.resourceSmoothingActive,
                ensureTile,
                ingestAvailability,
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
