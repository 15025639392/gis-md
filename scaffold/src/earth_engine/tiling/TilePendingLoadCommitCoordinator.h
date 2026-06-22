#pragma once

#include "TileContentUploadCommitter.h"
#include "TileEmptyContentRegistry.h"
#include "TileLoadLifecycle.h"
#include "TileLoadTypes.h"
#include "TilePendingUploadCompletion.h"
#include "TileTerminalLoadCommitter.h"
#include "TileGltfTerrainUpsampledChildMaterializer.h"
#include "TileTerrainUploadCommitter.h"
#include "TilesetTile.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace earth_engine {

class ActivatedRasterOverlay;
class RenderDevice;

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
              typename EnsureTileMeshFn,
              typename EnsureGltfResourcesFn,
              typename MarkResourcesDirtyFn>
    static void commitTerrainUpload(
        PendingTileLoad& upload,
        TilesetContentProvider* contentProvider,
        RenderDevice* device,
        const std::vector<ActivatedRasterOverlay*>& rasterOverlays,
        std::unordered_map<
            std::string,
            std::unique_ptr<DecodedHeightmap>>& terrainCache,
        TileLoadLifecycle& lifecycle,
        bool resourceSmoothingActive,
        EnsureTileFn&& ensureTile,
        EnsureTileMeshFn&& ensureTileMesh,
        EnsureGltfResourcesFn&& ensureGltfResources,
        MarkResourcesDirtyFn&& markResourcesDirty) {
        TileLoadedContent& content = upload.content();
        TileTerrainUploadCommitter::applyAvailabilityUpdates(
            contentProvider,
            content);
        const bool hadHeightmapTerrainPayload =
            content.terrainPayloadKind == TerrainTilePayloadKind::Heightmap &&
            content.heightmap != nullptr;
        TileTerrainUploadCommitter::cacheTerrainPayload(
            upload.cacheKey,
            content,
            terrainCache);
        bool uploadsGltfTerrain = content.hasGltfTerrainPayload();

        if (TilesetTile* tile = ensureTile(upload.key)) {
            TileGltfTerrainUpsampledChildMaterializer::materialize(
                *tile,
                content);
            uploadsGltfTerrain = content.hasGltfTerrainPayload();
            const bool uploadsHeightmapTerrain =
                !uploadsGltfTerrain &&
                content.terrainPayloadKind ==
                    TerrainTilePayloadKind::Heightmap &&
                hadHeightmapTerrainPayload;
            const bool uploadsParentUpsampledTerrain =
                !uploadsGltfTerrain &&
                !uploadsHeightmapTerrain &&
                tile->content.derivesTerrainFromParent();
            const bool uploadsTerrainPayload =
                uploadsGltfTerrain ||
                uploadsHeightmapTerrain ||
                uploadsParentUpsampledTerrain;
            if (uploadsTerrainPayload) {
                TileTerrainUploadCommitter::prepareTerrainRenderContent(
                    *tile,
                    std::move(content),
                    rasterOverlays,
                    device);
            }
            if (uploadsGltfTerrain) {
                ensureGltfResources(*tile);
            } else if ((uploadsHeightmapTerrain ||
                        uploadsParentUpsampledTerrain) &&
                       !resourceSmoothingActive &&
                       !tile->content.renderContent.hasSurfaceMesh()) {
                ensureTileMesh(*tile);
            }
            const bool resourcesReady = uploadsTerrainPayload &&
                (resourceSmoothingActive ||
                 (uploadsGltfTerrain
                      ? tile->content.renderContent.isRenderContentReady()
                      : tile->content.renderContent.hasSurfaceMesh()));
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
              typename EnsureTileMeshFn,
              typename EnsureGltfResourcesFn,
              typename MarkResourcesDirtyFn>
    static void commitUpload(
        PendingTileLoad& upload,
        TilesetContentProvider* contentProvider,
        RenderDevice* device,
        const std::vector<ActivatedRasterOverlay*>& rasterOverlays,
        std::unordered_map<
            std::string,
            std::unique_ptr<DecodedHeightmap>>& terrainCache,
        TileLoadLifecycle& lifecycle,
        bool resourceSmoothingActive,
        EnsureTileFn&& ensureTile,
        EnsureTileMeshFn&& ensureTileMesh,
        EnsureGltfResourcesFn&& ensureGltfResources,
        MarkResourcesDirtyFn&& markResourcesDirty) {
        if (upload.domain == TileLoadDomain::Content) {
            TileTerrainUploadCommitter::applyAvailabilityUpdates(
                contentProvider,
                upload.content());
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
                contentProvider,
                device,
                rasterOverlays,
                terrainCache,
                lifecycle,
                resourceSmoothingActive,
                std::forward<EnsureTileFn>(ensureTile),
                std::forward<EnsureTileMeshFn>(ensureTileMesh),
                std::forward<EnsureGltfResourcesFn>(ensureGltfResources),
                std::forward<MarkResourcesDirtyFn>(markResourcesDirty));
        }
    }
};

} // namespace earth_engine
