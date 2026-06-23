#pragma once

#include "TileContentUploadCommitter.h"
#include "TileEmptyContentRegistry.h"
#include "TileLoadLifecycle.h"
#include "TileLoadTypes.h"
#include "TilePendingUploadCompletion.h"
#include "TileTerminalLoadCommitter.h"
#include "TileTerrainUploadCommitter.h"
#include "TilesetTile.h"

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

    template <typename EnsureTileFn, typename MarkResourcesDirtyFn>
    static void commitTerrainTerminalResult(
        PendingTileLoad& result,
        TileEmptyContentRegistry& emptyContentRegistry,
        IPrepareRendererResources* pPrepRenderer,
        EnsureTileFn&& ensureTile,
        MarkResourcesDirtyFn&& markResourcesDirty) {
        TilesetTile* tile = ensureTile(result.key);
        if (!tile) return;

        const TileTerminalLoadAction action =
            TileTerminalLoadCommitter::commitTerrainTerminalResult(
                *tile,
                result.cacheKey,
                std::move(result.result),
                emptyContentRegistry,
                pPrepRenderer);
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
        if (!tile) return;

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
              typename EnsureTileMeshFn,
              typename MarkResourcesDirtyFn>
    static void commitTerrainUpload(
        PendingTileLoad& upload,
        RenderDevice* device,
        const std::vector<ActivatedRasterOverlay*>& rasterOverlays,
        std::unordered_map<
            std::string,
            std::unique_ptr<DecodedHeightmap>>& legacyTerrainCache,
        TileLoadLifecycle& lifecycle,
        bool resourceSmoothingActive,
        EnsureTileFn&& ensureTile,
        EnsureTileMeshFn&& ensureTileMesh,
        MarkResourcesDirtyFn&& markResourcesDirty) {
        TileLoadedContent& content = upload.content();
        const bool hadLegacyHeightmapTerrainPayload =
            content.terrainPayloadKind == TerrainTilePayloadKind::LegacyHeightmap &&
            content.heightmap != nullptr;
        TileTerrainUploadCommitter::cacheTerrainPayload(
            upload.cacheKey,
            content,
            legacyTerrainCache);

        if (TilesetTile* tile = ensureTile(upload.key)) {
            captureInitialBoundingVolumes(*tile, content.metadata);
            const bool uploadsLegacyHeightmapTerrain =
                content.terrainPayloadKind ==
                    TerrainTilePayloadKind::LegacyHeightmap &&
                hadLegacyHeightmapTerrainPayload;
            const bool uploadsParentUpsampledTerrain =
                !uploadsLegacyHeightmapTerrain &&
                tile->content.derivesTerrainFromParent();
            const bool uploadsTerrainPayload =
                uploadsLegacyHeightmapTerrain ||
                uploadsParentUpsampledTerrain;
            if (uploadsTerrainPayload) {
                TileTerrainUploadCommitter::prepareTerrainRenderContent(
                    *tile,
                    std::move(content),
                    rasterOverlays,
                    device);
            }
            if ((uploadsLegacyHeightmapTerrain ||
                 uploadsParentUpsampledTerrain) &&
                !resourceSmoothingActive &&
                !tile->content.renderContent.hasSurfaceMesh()) {
                ensureTileMesh(*tile);
            }
            const bool resourcesReady = uploadsTerrainPayload &&
                (resourceSmoothingActive ||
                 tile->content.renderContent.hasSurfaceMesh());
            const TileTerrainUploadCommitAction action =
                TileTerrainUploadCommitter::finishTerrainResourcePreparation(
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
        TilesetContentProvider* contentProvider,
        RenderDevice* device,
        IPrepareRendererResources* pPrepRenderer,
        const std::vector<ActivatedRasterOverlay*>& rasterOverlays,
        std::unordered_map<
            std::string,
            std::unique_ptr<DecodedHeightmap>>& legacyTerrainCache,
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

        const bool contentProviderOwnsTerrainQuadtree =
            contentProvider && contentProvider->providesTerrainQuadtree();
        if (!contentProviderOwnsTerrainQuadtree) {
            legacyTerrainCache.erase(upload.cacheKey);
        }
        captureInitialBoundingVolumes(*tile, upload.content().metadata);
        TileContentUploadCommitter::applyAvailabilityUpdates(
            contentProvider,
            upload.content());
        TileContentUploadCommitter::prepareRenderContent(
            *tile,
            std::move(upload.content()),
            rasterOverlays,
            device,
            pPrepRenderer);
        ensureGltfResources(*tile);
        const TileContentUploadCommitAction action =
            TileContentUploadCommitter::finishRenderResourcePreparation(
                *tile,
                tile->content.renderContent.isRenderContentReady());
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
        IPrepareRendererResources* pPrepRenderer,
        EnsureTileFn&& ensureTile,
        EnsureChildrenFn&& ensureChildren,
        MarkResourcesDirtyFn&& markResourcesDirty) {
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
                std::forward<MarkResourcesDirtyFn>(markResourcesDirty));
        }
    }

    template <typename EnsureTileFn,
              typename EnsureTileMeshFn,
              typename EnsureGltfResourcesFn,
              typename MarkResourcesDirtyFn>
    static void commitUpload(
        PendingTileLoad& upload,
        TilesetContentProvider* contentProvider,
        RenderDevice* device,
        IPrepareRendererResources* pPrepRenderer,
        const std::vector<ActivatedRasterOverlay*>& rasterOverlays,
        std::unordered_map<
            std::string,
            std::unique_ptr<DecodedHeightmap>>& legacyTerrainCache,
        TileLoadLifecycle& lifecycle,
        bool resourceSmoothingActive,
        EnsureTileFn&& ensureTile,
        EnsureTileMeshFn&& ensureTileMesh,
        EnsureGltfResourcesFn&& ensureGltfResources,
        MarkResourcesDirtyFn&& markResourcesDirty) {
        if (upload.domain == TileLoadDomain::Content) {
            commitContentUpload(
                upload,
                contentProvider,
                device,
                pPrepRenderer,
                rasterOverlays,
                legacyTerrainCache,
                lifecycle,
                std::forward<EnsureTileFn>(ensureTile),
                std::forward<EnsureGltfResourcesFn>(ensureGltfResources),
                std::forward<MarkResourcesDirtyFn>(markResourcesDirty));
        } else {
            commitTerrainUpload(
                upload,
                device,
                rasterOverlays,
                legacyTerrainCache,
                lifecycle,
                resourceSmoothingActive,
                std::forward<EnsureTileFn>(ensureTile),
                std::forward<EnsureTileMeshFn>(ensureTileMesh),
                std::forward<MarkResourcesDirtyFn>(markResourcesDirty));
        }
    }
};

} // namespace earth_engine
