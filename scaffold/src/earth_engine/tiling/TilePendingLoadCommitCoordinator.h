#pragma once

#include "TileContentUploadCommitter.h"
#include "TileEmptyContentRegistry.h"
#include "TileLoadLifecycle.h"
#include "TileLoadResultMetadataApplicator.h"
#include "TileLoadTypes.h"
#include "TilePendingUploadCompletion.h"
#include "TileTerminalLoadCommitter.h"
#include "TileTerrainUploadCommitter.h"
#include "TilesetTile.h"

#include "../providers/QuantizedMeshTerrainProvider.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

namespace earth_engine {

class TilePendingLoadCommitCoordinator {
public:
    template <typename EnsureTileFn, typename MarkResourcesDirtyFn>
    static void commitTerrainTerminalResult(
        const PendingTerrainTerminalResult& result,
        TileEmptyContentRegistry& emptyContentRegistry,
        EnsureTileFn&& ensureTile,
        MarkResourcesDirtyFn&& markResourcesDirty) {
        TilesetTile* tile = ensureTile(result.key);
        if (!tile) return;

        const TileTerminalLoadAction action =
            TileTerminalLoadCommitter::commitTerrainTerminalResult(
                *tile,
                result.cacheKey,
                result.status,
                emptyContentRegistry);
        if (action.resourcesDirty) {
            markResourcesDirty();
        }
    }

    template <typename EnsureTileFn,
              typename EnsureChildrenFn,
              typename MarkResourcesDirtyFn>
    static void commitContentTerminalResult(
        const PendingContentTerminalResult& result,
        TileEmptyContentRegistry& emptyContentRegistry,
        EnsureTileFn&& ensureTile,
        EnsureChildrenFn&& ensureChildren,
        MarkResourcesDirtyFn&& markResourcesDirty) {
        TilesetTile* tile = ensureTile(result.key);
        if (!tile) return;

        const TileTerminalLoadAction action =
            TileTerminalLoadCommitter::commitContentTerminalResult(
                *tile,
                result.cacheKey,
                result.status,
                emptyContentRegistry);
        if (action.ensureChildren) {
            ensureChildren(*tile);
        }
        if (action.resourcesDirty) {
            markResourcesDirty();
        }
    }

    template <typename EnsureTileFn,
              typename IngestAvailabilityFn,
              typename EnsureTileMeshFn,
              typename MarkResourcesDirtyFn>
    static void commitTerrainUpload(
        PendingTerrainUpload& upload,
        TerrainProvider* terrainProvider,
        std::unordered_map<
            std::string,
            std::unique_ptr<DecodedHeightmap>>& terrainCache,
        TileLoadLifecycle& lifecycle,
        bool resourceSmoothingActive,
        EnsureTileFn&& ensureTile,
        IngestAvailabilityFn&& ingestAvailability,
        EnsureTileMeshFn&& ensureTileMesh,
        MarkResourcesDirtyFn&& markResourcesDirty) {
        if (!upload.quantizedMeshAvailabilityUpdates.empty()) {
            if (auto* qmProvider =
                    dynamic_cast<QuantizedMeshTerrainProvider*>(
                        terrainProvider)) {
                qmProvider->applyAvailabilityUpdates(
                    upload.quantizedMeshAvailabilityUpdates);
            }
        }
        if (upload.heightmap) {
            ingestAvailability(
                upload.key,
                upload.heightmap.get(),
                upload.surfaceMesh.get());
            terrainCache[upload.cacheKey] = std::move(upload.heightmap);
        } else if (upload.surfaceMesh) {
            ingestAvailability(
                upload.key,
                nullptr,
                upload.surfaceMesh.get());
        }

        if (TilesetTile* tile = ensureTile(upload.key)) {
            if (upload.surfaceMesh &&
                !tile->content.renderContent.hasSurfaceMesh()) {
                tile->content.renderContent.setSurfaceMesh(
                    std::move(upload.surfaceMesh));
            }
            TileLoadResultMetadataApplicator::apply(
                *tile,
                std::move(upload.metadata));
            TileTerrainUploadCommitter::prepareTerrainRenderContent(*tile);
            if (!resourceSmoothingActive &&
                !tile->content.renderContent.hasSurfaceMesh()) {
                ensureTileMesh(*tile);
            }
            const bool resourcesReady =
                resourceSmoothingActive || tile->content.renderContent.isMeshReady();
            const TileTerrainUploadCommitAction action =
                TileTerrainUploadCommitter::finishMeshResourcePreparation(
                    *tile,
                    resourcesReady);
            if (action.resourcesDirty) {
                markResourcesDirty();
            }
        }

        TilePendingUploadCompletion::eraseTerrainUpload(
            lifecycle,
            upload.cacheKey);
    }

    template <typename EnsureTileFn,
              typename EnsureGltfResourcesFn,
              typename MarkResourcesDirtyFn>
    static void commitContentUpload(
        PendingContentUpload& upload,
        std::unordered_map<
            std::string,
            std::unique_ptr<DecodedHeightmap>>& terrainCache,
        TileLoadLifecycle& lifecycle,
        EnsureTileFn&& ensureTile,
        EnsureGltfResourcesFn&& ensureGltfResources,
        MarkResourcesDirtyFn&& markResourcesDirty) {
        TilesetTile* tile = ensureTile(upload.key);
        if (!tile) {
            TilePendingUploadCompletion::eraseContentUpload(
                lifecycle,
                upload.cacheKey);
            return;
        }

        terrainCache.erase(upload.cacheKey);
        TileContentUploadCommitter::prepareRenderContent(
            *tile,
            std::move(upload.result));
        ensureGltfResources(*tile);
        const TileContentUploadCommitAction action =
            TileContentUploadCommitter::finishRenderResourcePreparation(
                *tile,
                tile->content.renderContent.isMeshReady());
        if (action.resourcesDirty) {
            markResourcesDirty();
        }

        TilePendingUploadCompletion::eraseContentUpload(
            lifecycle,
            upload.cacheKey);
    }
};

} // namespace earth_engine
