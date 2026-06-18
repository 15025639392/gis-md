#pragma once

#include "TileEmptyContentRegistry.h"
#include "TileLoadLifecycle.h"
#include "TilePendingLoadCommitCoordinator.h"
#include "TilePendingLoadProcessor.h"

#include <memory>
#include <string>
#include <unordered_map>

namespace earth_engine {

class TerrainProvider;
struct DecodedHeightmap;
struct TileKey;
struct TilesetTile;

struct TilePendingUploadFrameProcessorInput {
    TileLoadLifecycle& loadLifecycle;
    FrameResourceBudget& budget;
    TerrainProvider* terrainProvider = nullptr;
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
        auto processTerrainTerminalResult =
            [&](const PendingTerrainTerminalResult& result) {
                TilePendingLoadCommitCoordinator::commitTerrainTerminalResult(
                    result,
                    input.emptyContentRegistry,
                    ensureTile,
                    markResourcesDirty);
            };

        auto processTerrainUpload = [&](PendingTerrainUpload& upload) {
            TilePendingLoadCommitCoordinator::commitTerrainUpload(
                upload,
                input.terrainProvider,
                input.terrainCache,
                input.loadLifecycle,
                input.resourceSmoothingActive,
                ensureTile,
                ingestAvailability,
                ensureTileMesh,
                markResourcesDirty);
        };

        auto processContentTerminalResult =
            [&](const PendingContentTerminalResult& result) {
                TilePendingLoadCommitCoordinator::commitContentTerminalResult(
                    result,
                    input.emptyContentRegistry,
                    ensureTile,
                    ensureTileChildren,
                    markResourcesDirty);
            };

        auto processContentUpload = [&](PendingContentUpload& upload) {
            TilePendingLoadCommitCoordinator::commitContentUpload(
                upload,
                input.terrainCache,
                input.loadLifecycle,
                ensureTile,
                ensureGltfResources,
                markResourcesDirty);
        };

        return TilePendingLoadProcessor::processPendingLoads(
            TilePendingLoadProcessorInput{
                input.loadLifecycle,
                input.budget,
                input.interactionActive},
            processTerrainTerminalResult,
            processContentTerminalResult,
            processTerrainUpload,
            processContentUpload);
    }
};

} // namespace earth_engine
