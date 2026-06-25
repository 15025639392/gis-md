#pragma once

#include "TileContentTerrainResiduePolicy.h"
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
    IPrepareRendererResources* pPrepRenderer = nullptr;
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
        TilesetTile& tile = input.tile;
        if (TileContentTerrainResiduePolicy::clearRejectableResidue(
                tile,
                input.pPrepRenderer)) {
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
        IngestAvailabilityFn&& ingestAvailability,
        FindUpsampleSourceFn&& findUpsampleSource,
        EnsureAncestorMeshFn&& ensureAncestorMesh,
        IsCompleteRenderableFn&& isCompleteRenderable,
        MarkResourcesDirtyFn&& markResourcesDirty) {
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
                    true},
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
