#pragma once

#include "TileContentUploadCommitter.h"
#include "TileEmptyContentRegistry.h"
#include "TileLoadLifecycle.h"
#include "TileLoadTypes.h"
#include "TilePendingUploadCompletion.h"
#include "TileTerminalLoadCommitter.h"
#include "TilesetTile.h"
#include "../content/GltfContentProvider.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace earth_engine {

class ActivatedRasterOverlay;
class IPrepareRendererResources;
class RenderDevice;

class TilePendingLoadCommitCoordinator {
public:
    static void captureInitialBoundingVolumes(
        TilesetTile& tile,
        TileLoadResultMetadata& metadata) {
        if (!metadata.initialBoundingVolume && tile.boundingVolume) {
            metadata.initialBoundingVolume = *tile.boundingVolume;
        }
        if (!metadata.initialContentBoundingVolume &&
            tile.contentBoundingVolume) {
            metadata.initialContentBoundingVolume =
                *tile.contentBoundingVolume;
        }
    }

    template <typename EnsureTileFn,
              typename EnsureChildrenFn,
              typename MarkResourcesDirtyFn>
    static void commitTerrainTerminalResult(
        PendingTileLoad& result,
        TileEmptyContentRegistry& emptyContentRegistry,
        IPrepareRendererResources* pPrepRenderer,
        EnsureTileFn&& ensureTile,
        EnsureChildrenFn&& ensureChildren,
        MarkResourcesDirtyFn&& markResourcesDirty,
        TilesetContentProvider* contentProvider = nullptr) {
        TilesetTile* tile = ensureTile(result.key);
        if (!tile) {
            emptyContentRegistry.erase(result.cacheKey);
            return;
        }

        const TileTerminalLoadAction action =
            TileTerminalLoadCommitter::commitTerrainTerminalResult(
                *tile,
                result.cacheKey,
                std::move(result.result),
                emptyContentRegistry,
                pPrepRenderer,
                contentProvider);
        if (action.ensureChildren) {
            ensureChildren(*tile);
        }
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
        IPrepareRendererResources* pPrepRenderer,
        EnsureTileFn&& ensureTile,
        EnsureChildrenFn&& ensureChildren,
        MarkResourcesDirtyFn&& markResourcesDirty) {
        TilesetTile* tile = ensureTile(result.key);
        if (!tile) {
            emptyContentRegistry.erase(result.cacheKey);
            return;
        }

        const TileTerminalLoadAction action =
            TileTerminalLoadCommitter::commitContentTerminalResult(
                *tile,
                result.cacheKey,
                std::move(result.result),
                emptyContentRegistry,
                pPrepRenderer);
        if (action.ensureChildren) {
            ensureChildren(*tile);
        }
        if (action.resourcesDirty) {
            markResourcesDirty();
        }
    }

    template <typename EnsureTileFn,
              typename EnsureChildrenFn,
              typename EnsureGltfResourcesFn,
              typename MarkResourcesDirtyFn>
    static void commitContentUpload(
        PendingTileLoad& upload,
        TilesetContentProvider* contentProvider,
        RenderDevice* device,
        IPrepareRendererResources* pPrepRenderer,
        const std::vector<ActivatedRasterOverlay*>& rasterOverlays,
        TileEmptyContentRegistry& emptyContentRegistry,
        TileLoadLifecycle& lifecycle,
        EnsureTileFn&& ensureTile,
        EnsureChildrenFn&& ensureChildren,
        EnsureGltfResourcesFn&& ensureGltfResources,
        MarkResourcesDirtyFn&& markResourcesDirty) {
        TilesetTile* tile = ensureTile(upload.key);
        if (!tile) {
            emptyContentRegistry.erase(upload.cacheKey);
            TilePendingUploadCompletion::eraseUpload(
                lifecycle,
                upload.cacheKey);
            return;
        }

        if (upload.result.shouldFailUploadForDomain(upload.domain)) {
            TileLoadResult failedResult =
                TileLoadResult::createFailedPreservingAvailability(
                    std::move(upload.result));
            const TileTerminalLoadAction action =
                upload.domain == TileLoadDomain::TerrainContent
                    ? TileTerminalLoadCommitter::commitTerrainTerminalResult(
                          *tile,
                          upload.cacheKey,
                          std::move(failedResult),
                          emptyContentRegistry,
                          pPrepRenderer,
                          contentProvider)
                    : TileTerminalLoadCommitter::commitContentTerminalResult(
                          *tile,
                          upload.cacheKey,
                          std::move(failedResult),
                          emptyContentRegistry,
                          pPrepRenderer);
            if (action.ensureChildren) {
                ensureChildren(*tile);
            }
            if (action.resourcesDirty) {
                markResourcesDirty();
            }
            TilePendingUploadCompletion::eraseUpload(
                lifecycle,
                upload.cacheKey);
            return;
        }

        captureInitialBoundingVolumes(*tile, upload.content().metadata);
        const bool shouldApplyTerrainAvailability =
            upload.content().satisfiesContentTerrainPayloadContract() &&
            !upload.content().quantizedMeshAvailabilityUpdates.empty() &&
            contentProvider &&
            contentProvider->providesTerrainQuadtree();
        if (shouldApplyTerrainAvailability) {
            contentProvider->applyTerrainAvailabilityUpdates(
                upload.content().quantizedMeshAvailabilityUpdates);
        }
        TileContentUploadCommitter::prepareRenderContent(
            *tile,
            std::move(upload.content()),
            rasterOverlays,
            device,
            pPrepRenderer);
        ensureGltfResources(*tile);
        const bool renderResourcesReady =
            tile->content.renderContent.isRenderContentReady();
        const TileContentUploadCommitAction action =
            TileContentUploadCommitter::finishRenderResourcePreparation(
                *tile,
                renderResourcesReady,
                pPrepRenderer);
        if (action.ensureChildren) {
            ensureChildren(*tile);
        }
        if (action.resourcesDirty) {
            markResourcesDirty();
        }

        emptyContentRegistry.erase(upload.cacheKey);
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
        IPrepareRendererResources* pPrepRenderer,
        EnsureTileFn&& ensureTile,
        EnsureChildrenFn&& ensureChildren,
        MarkResourcesDirtyFn&& markResourcesDirty,
        TilesetContentProvider* contentProvider = nullptr) {
        if (result.domain == TileLoadDomain::Content) {
            commitContentTerminalResult(
                result,
                emptyContentRegistry,
                pPrepRenderer,
                std::forward<EnsureTileFn>(ensureTile),
                std::forward<EnsureChildrenFn>(ensureChildren),
                std::forward<MarkResourcesDirtyFn>(markResourcesDirty));
        } else {
            commitTerrainTerminalResult(
                result,
                emptyContentRegistry,
                pPrepRenderer,
                std::forward<EnsureTileFn>(ensureTile),
                std::forward<EnsureChildrenFn>(ensureChildren),
                std::forward<MarkResourcesDirtyFn>(markResourcesDirty),
                contentProvider);
        }
    }

    template <typename EnsureTileFn,
              typename EnsureChildrenFn,
              typename EnsureGltfResourcesFn,
              typename MarkResourcesDirtyFn>
    static void commitUpload(
        PendingTileLoad& upload,
        TilesetContentProvider* contentProvider,
        RenderDevice* device,
        IPrepareRendererResources* pPrepRenderer,
        const std::vector<ActivatedRasterOverlay*>& rasterOverlays,
        TileEmptyContentRegistry& emptyContentRegistry,
        TileLoadLifecycle& lifecycle,
        EnsureTileFn&& ensureTile,
        EnsureChildrenFn&& ensureChildren,
        EnsureGltfResourcesFn&& ensureGltfResources,
        MarkResourcesDirtyFn&& markResourcesDirty) {
        commitContentUpload(
            upload,
            contentProvider,
            device,
            pPrepRenderer,
            rasterOverlays,
            emptyContentRegistry,
            lifecycle,
            std::forward<EnsureTileFn>(ensureTile),
            std::forward<EnsureChildrenFn>(ensureChildren),
            std::forward<EnsureGltfResourcesFn>(ensureGltfResources),
            std::forward<MarkResourcesDirtyFn>(markResourcesDirty));
    }

};

} // namespace earth_engine
