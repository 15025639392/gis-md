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
        PendingTileLoad& result,
        TileEmptyContentRegistry& emptyContentRegistry,
        EnsureTileFn&& ensureTile,
        MarkResourcesDirtyFn&& markResourcesDirty) {
        TilesetTile* tile = ensureTile(result.key);
        if (!tile) return;

        const TileTerminalLoadAction action =
            TileTerminalLoadCommitter::commitTerrainTerminalResult(
                *tile,
                result.cacheKey,
              std::move(result.result),
              emptyContentRegistry);
        if (action.resourcesDirty) {
            markResourcesDirty();
        }
    }

    template <typename EnsureTileFn,
              typename EnsureChildrenFn,
              typename MarkResourcesDirtyFn>
    static void commitContentTerminalResult(
        PendingTileLoad& result,
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
                std::move(result.result),
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
        PendingTileLoad& upload,
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
        TileLoadedContent& content = upload.content();
        if (!content.quantizedMeshAvailabilityUpdates.empty()) {
            if (auto* qmProvider =
                    dynamic_cast<QuantizedMeshTerrainProvider*>(
                        terrainProvider)) {
                qmProvider->applyAvailabilityUpdates(
                    content.quantizedMeshAvailabilityUpdates);
            }
        }
        if (content.heightmap) {
            ingestAvailability(
                upload.key,
                content.heightmap.get(),
                content.surfaceMesh.get());
            terrainCache[upload.cacheKey] = std::move(content.heightmap);
        } else if (content.surfaceMesh) {
            ingestAvailability(
                upload.key,
                nullptr,
                content.surfaceMesh.get());
        }

        if (TilesetTile* tile = ensureTile(upload.key)) {
            if (content.surfaceMesh &&
                !tile->content.renderContent.hasSurfaceMesh()) {
                tile->content.renderContent.setSurfaceMesh(
                    std::move(content.surfaceMesh));
            }
            TileLoadResultMetadataApplicator::apply(
                *tile,
                std::move(content.metadata));
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

        TilePendingUploadCompletion::eraseUpload(
            lifecycle,
            upload.cacheKey);
    }

    template <typename EnsureTileFn,
              typename EnsureGltfResourcesFn,
              typename MarkResourcesDirtyFn>
    static void commitContentUpload(
        PendingTileLoad& upload,
        std::unordered_map<
            std::string,
            std::unique_ptr<DecodedHeightmap>>& terrainCache,
        TileLoadLifecycle& lifecycle,
        EnsureTileFn&& ensureTile,
        EnsureGltfResourcesFn&& ensureGltfResources,
        MarkResourcesDirtyFn&& markResourcesDirty) {
        TilesetTile* tile = ensureTile(upload.key);
        if (!tile) {
            TilePendingUploadCompletion::eraseUpload(
                lifecycle,
                upload.cacheKey);
            return;
        }

        terrainCache.erase(upload.cacheKey);
        TileContentUploadCommitter::prepareRenderContent(
            *tile,
            std::move(upload.content()));
        ensureGltfResources(*tile);
        const TileContentUploadCommitAction action =
            TileContentUploadCommitter::finishRenderResourcePreparation(
                *tile,
                tile->content.renderContent.isMeshReady());
        if (action.resourcesDirty) {
            markResourcesDirty();
        }

        TilePendingUploadCompletion::eraseUpload(
            lifecycle,
            upload.cacheKey);
    }

    template <typename EnsureTileFn,
              typename EnsureChildrenFn,
              typename MarkResourcesDirtyFn>
    static void commitTerminalResult(
        PendingTileLoad& result,
        TileEmptyContentRegistry& emptyContentRegistry,
        EnsureTileFn&& ensureTile,
        EnsureChildrenFn&& ensureChildren,
        MarkResourcesDirtyFn&& markResourcesDirty) {
        if (result.domain == TileLoadDomain::Content) {
            commitContentTerminalResult(
                result,
                emptyContentRegistry,
                std::forward<EnsureTileFn>(ensureTile),
                std::forward<EnsureChildrenFn>(ensureChildren),
                std::forward<MarkResourcesDirtyFn>(markResourcesDirty));
        } else {
            commitTerrainTerminalResult(
                result,
                emptyContentRegistry,
                std::forward<EnsureTileFn>(ensureTile),
                std::forward<MarkResourcesDirtyFn>(markResourcesDirty));
        }
    }

    template <typename EnsureTileFn,
              typename IngestAvailabilityFn,
              typename EnsureTileMeshFn,
              typename EnsureGltfResourcesFn,
              typename MarkResourcesDirtyFn>
    static void commitUpload(
        PendingTileLoad& upload,
        TerrainProvider* terrainProvider,
        std::unordered_map<
            std::string,
            std::unique_ptr<DecodedHeightmap>>& terrainCache,
        TileLoadLifecycle& lifecycle,
        bool resourceSmoothingActive,
        EnsureTileFn&& ensureTile,
        IngestAvailabilityFn&& ingestAvailability,
        EnsureTileMeshFn&& ensureTileMesh,
        EnsureGltfResourcesFn&& ensureGltfResources,
        MarkResourcesDirtyFn&& markResourcesDirty) {
        if (upload.domain == TileLoadDomain::Content) {
            commitContentUpload(
                upload,
                terrainCache,
                lifecycle,
                std::forward<EnsureTileFn>(ensureTile),
                std::forward<EnsureGltfResourcesFn>(ensureGltfResources),
                std::forward<MarkResourcesDirtyFn>(markResourcesDirty));
        } else {
            commitTerrainUpload(
                upload,
                terrainProvider,
                terrainCache,
                lifecycle,
                resourceSmoothingActive,
                std::forward<EnsureTileFn>(ensureTile),
                std::forward<IngestAvailabilityFn>(ingestAvailability),
                std::forward<EnsureTileMeshFn>(ensureTileMesh),
                std::forward<MarkResourcesDirtyFn>(markResourcesDirty));
        }
    }
};

} // namespace earth_engine
