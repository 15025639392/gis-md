#pragma once

#include "TileContentUploadCommitter.h"
#include "TileEmptyContentRegistry.h"
#include "TileAvailabilityUpdateCommitter.h"
#include "TileLoadDomainPolicy.h"
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
    static void commitTerminalResult(
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
            TileTerminalLoadCommitter::commitTerminalResult(
                result.domain,
                *tile,
                result.cacheKey,
                std::move(result.result),
                emptyContentRegistry,
                pPrepRenderer,
                contentProvider);
        applyCommitAction(action, *tile, ensureChildren, markResourcesDirty);
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
        TilesetTile* tile = ensureTile(upload.key);
        if (!tile) {
            emptyContentRegistry.erase(upload.cacheKey);
            TilePendingUploadCompletion::eraseUpload(
                lifecycle,
                upload.cacheKey);
            return;
        }

        if (TileLoadDomainPolicy::shouldFailUploadForDomain(
                upload.domain,
                upload.result)) {
            TileLoadResult failedResult =
                TileLoadDomainPolicy::normalizeForDomain(
                    upload.domain,
                    std::move(upload.result));
            const TileTerminalLoadAction action =
                TileTerminalLoadCommitter::commitTerminalResult(
                    upload.domain,
                    *tile,
                    upload.cacheKey,
                    std::move(failedResult),
                    emptyContentRegistry,
                    pPrepRenderer,
                    contentProvider);
            applyCommitAction(action, *tile, ensureChildren, markResourcesDirty);
            TilePendingUploadCompletion::eraseUpload(
                lifecycle,
                upload.cacheKey);
            return;
        }

        captureInitialBoundingVolumes(*tile, upload.content().metadata);
        TileAvailabilityUpdateCommitter::applyTerrainAvailabilityUpdates(
            upload.domain,
            upload.result,
            contentProvider);
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
        applyCommitAction(action, *tile, ensureChildren, markResourcesDirty);

        emptyContentRegistry.erase(upload.cacheKey);
        TilePendingUploadCompletion::eraseUpload(
            lifecycle,
            upload.cacheKey);
    }

private:
    template <typename CommitAction,
              typename EnsureChildrenFn,
              typename MarkResourcesDirtyFn>
    static void applyCommitAction(
        const CommitAction& action,
        TilesetTile& tile,
        EnsureChildrenFn&& ensureChildren,
        MarkResourcesDirtyFn&& markResourcesDirty) {
        if (action.ensureChildren) {
            ensureChildren(tile);
        }
        if (action.resourcesDirty) {
            markResourcesDirty();
        }
    }

};

} // namespace earth_engine
