#pragma once

#include "TileContentUploadCommitter.h"
#include "TileEmptyContentRegistry.h"
#include "TileLoadLifecycle.h"
#include "TileLoadTypes.h"
#include "TilePendingUploadCompletion.h"
#include "TileTerminalLoadCommitter.h"
#include "TileTerrainUploadCommitter.h"
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
    static bool shouldDiscardLegacyHeightmapTerrainAdapter(
        const TilesetContentProvider* contentProvider,
        TileLoadDomain domain) {
        return domain == TileLoadDomain::HeightmapTerrainAdapter &&
               contentProvider &&
               contentProvider->providesTerrainQuadtree();
    }

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
    static void commitLegacyHeightmapTerrainAdapterUpload(
        PendingTileLoad& upload,
        RenderDevice* device,
        const std::vector<ActivatedRasterOverlay*>& rasterOverlays,
        std::unordered_map<
            std::string,
            std::unique_ptr<DecodedHeightmap>>& heightmapTerrainAdapterCache,
        TileLoadLifecycle& lifecycle,
        bool resourceSmoothingActive,
        EnsureTileFn&& ensureTile,
        EnsureTileMeshFn&& ensureTileMesh,
        MarkResourcesDirtyFn&& markResourcesDirty) {
        TileLoadedContent& content = upload.content();
        const bool hadHeightmapTerrainAdapterPayload =
            content.terrainPayloadKind == TerrainTilePayloadKind::LegacyHeightmap &&
            content.heightmap != nullptr;
        TileTerrainUploadCommitter::cacheTerrainPayload(
            upload.cacheKey,
            content,
            heightmapTerrainAdapterCache);

        if (TilesetTile* tile = ensureTile(upload.key)) {
            captureInitialBoundingVolumes(*tile, content.metadata);
            const bool uploadsHeightmapTerrainAdapter =
                content.terrainPayloadKind ==
                    TerrainTilePayloadKind::LegacyHeightmap &&
                hadHeightmapTerrainAdapterPayload;
            const bool uploadsParentUpsampledTerrain =
                !uploadsHeightmapTerrainAdapter &&
                tile->content.derivesTerrainFromParent();
            const bool uploadsTerrainPayload =
                uploadsHeightmapTerrainAdapter ||
                uploadsParentUpsampledTerrain;
            if (uploadsTerrainPayload) {
                TileTerrainUploadCommitter::prepareTerrainRenderContent(
                    *tile,
                    std::move(content),
                    rasterOverlays,
                    device);
            }
            if ((uploadsHeightmapTerrainAdapter ||
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
        TilesetContentProvider* contentProvider,
        TileEmptyContentRegistry& emptyContentRegistry,
        IPrepareRendererResources* pPrepRenderer,
        EnsureTileFn&& ensureTile,
        EnsureChildrenFn&& ensureChildren,
        MarkResourcesDirtyFn&& markResourcesDirty) {
        if (shouldDiscardLegacyHeightmapTerrainAdapter(
                contentProvider,
                result.domain)) {
            return;
        }
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
            std::unique_ptr<DecodedHeightmap>>& heightmapTerrainAdapterCache,
        TileLoadLifecycle& lifecycle,
        bool resourceSmoothingActive,
        EnsureTileFn&& ensureTile,
        EnsureTileMeshFn&& ensureTileMesh,
        EnsureGltfResourcesFn&& ensureGltfResources,
        MarkResourcesDirtyFn&& markResourcesDirty) {
        if (shouldDiscardLegacyHeightmapTerrainAdapter(
                contentProvider,
                upload.domain)) {
            TilePendingUploadCompletion::eraseUpload(
                lifecycle,
                upload.cacheKey);
            return;
        }
        if (isContentLoadDomain(upload.domain)) {
            commitContentUpload(
                upload,
                contentProvider,
                device,
                pPrepRenderer,
                rasterOverlays,
                lifecycle,
                std::forward<EnsureTileFn>(ensureTile),
                std::forward<EnsureGltfResourcesFn>(ensureGltfResources),
                std::forward<MarkResourcesDirtyFn>(markResourcesDirty));
        } else {
            commitLegacyHeightmapTerrainAdapterUpload(
                upload,
                device,
                rasterOverlays,
                heightmapTerrainAdapterCache,
                lifecycle,
                resourceSmoothingActive,
                std::forward<EnsureTileFn>(ensureTile),
                std::forward<EnsureTileMeshFn>(ensureTileMesh),
                std::forward<MarkResourcesDirtyFn>(markResourcesDirty));
        }
    }
};

} // namespace earth_engine
