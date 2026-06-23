#pragma once

#include "TileSurfaceMeshEnsurer.h"

#include <memory>
#include <string>
#include <unordered_map>

namespace earth_engine {

struct DecodedHeightmap;
class RenderDevice;
struct TileKey;
struct TilesetTile;

struct TileHeightmapMeshFrameEnsureInput {
    TilesetTile& tile;
    std::unordered_map<std::string, std::unique_ptr<DecodedHeightmap>>&
        terrainCache;
    RenderDevice* device = nullptr;
    bool hasTerrainQuadtree = false;
};

struct TileContentTerrainMeshFrameEnsureInput {
    TilesetTile& tile;
    RenderDevice* device = nullptr;
    bool hasTerrainQuadtree = true;
};

class TileMeshFrameEnsurer {
public:
    template <typename TerrainCacheKeyFn,
              typename IngestAvailabilityFn,
              typename FindUpsampleSourceFn,
              typename EnsureAncestorMeshFn,
              typename IsCompleteRenderableFn,
              typename MarkResourcesDirtyFn>
    static void ensureHeightmapSurface(
        const TileHeightmapMeshFrameEnsureInput& input,
        TerrainCacheKeyFn&& terrainCacheKey,
        IngestAvailabilityFn&& ingestAvailability,
        FindUpsampleSourceFn&& findUpsampleSource,
        EnsureAncestorMeshFn&& ensureAncestorMesh,
        IsCompleteRenderableFn&& isCompleteRenderable,
        MarkResourcesDirtyFn&& markResourcesDirty) {
        auto it = input.terrainCache.find(terrainCacheKey(input.tile.key));
        DecodedHeightmap* ownHeightmap =
            it != input.terrainCache.end() && it->second
                ? it->second.get()
                : nullptr;
        ensureSurface(
            input.tile,
            ownHeightmap,
            input.device,
            input.hasTerrainQuadtree,
            true,
            std::forward<IngestAvailabilityFn>(ingestAvailability),
            std::forward<FindUpsampleSourceFn>(findUpsampleSource),
            std::forward<EnsureAncestorMeshFn>(ensureAncestorMesh),
            std::forward<IsCompleteRenderableFn>(isCompleteRenderable),
            std::forward<MarkResourcesDirtyFn>(markResourcesDirty));
    }

    template <typename MarkResourcesDirtyFn>
    static void ensureContentTerrain(
        const TileContentTerrainMeshFrameEnsureInput& input,
        MarkResourcesDirtyFn&& markResourcesDirty) {
        (void)input.device;
        (void)input.hasTerrainQuadtree;

        TilesetTile& tile = input.tile;
        const bool hasHeightmapSurfaceResidue =
            tile.content.renderContent.hasSurfaceMesh() ||
            tile.content.renderContent.hasRetainedHeightmap();
        if (hasHeightmapSurfaceResidue) {
            tile.content.renderContent.clearSurfaceMeshResources();
            tile.content.renderContent.clearRetainedHeightmap();
            tile.rasterOverlayState.releaseAndClearReferences(nullptr);
            markResourcesDirty();
        }
    }

private:
    template <typename IngestAvailabilityFn,
              typename FindUpsampleSourceFn,
              typename EnsureAncestorMeshFn,
              typename IsCompleteRenderableFn,
              typename MarkResourcesDirtyFn>
    static void ensureSurface(
        TilesetTile& tile,
        DecodedHeightmap* ownHeightmap,
        RenderDevice* device,
        bool hasTerrainQuadtree,
        bool useHeightmapSurfacePath,
        IngestAvailabilityFn&& ingestAvailability,
        FindUpsampleSourceFn&& findUpsampleSource,
        EnsureAncestorMeshFn&& ensureAncestorMesh,
        IsCompleteRenderableFn&& isCompleteRenderable,
        MarkResourcesDirtyFn&& markResourcesDirty) {
        const bool contentTerrainQuadtreeOwnsSurface =
            hasTerrainQuadtree && !useHeightmapSurfacePath;
        const bool hasHeightmapSurfaceResidue =
            contentTerrainQuadtreeOwnsSurface &&
            (tile.content.renderContent.hasSurfaceMesh() ||
             tile.content.renderContent.hasRetainedHeightmap());
        if (hasHeightmapSurfaceResidue) {
            tile.content.renderContent.clearSurfaceMeshResources();
            tile.content.renderContent.clearRetainedHeightmap();
            markResourcesDirty();
        }

        if (tile.content.renderContent.hasGltfContent()) {
            return;
        }

        const TileSurfaceMeshEnsureResult result =
            TileSurfaceMeshEnsurer::ensure(
                TileSurfaceMeshEnsureInput{
                    tile,
                    ownHeightmap,
                    device,
                    hasTerrainQuadtree,
                    useHeightmapSurfacePath},
                ingestAvailability,
                findUpsampleSource,
                ensureAncestorMesh,
                isCompleteRenderable);
        if (result.resourcesDirty) {
            markResourcesDirty();
        }
    }
};

} // namespace earth_engine
