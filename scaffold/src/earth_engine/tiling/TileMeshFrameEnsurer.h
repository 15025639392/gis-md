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

struct TileMeshFrameEnsureInput {
    TilesetTile& tile;
    std::unordered_map<std::string, std::unique_ptr<DecodedHeightmap>>&
        terrainCache;
    RenderDevice* device = nullptr;
    bool hasTerrainQuadtree = false;
    bool allowLegacyHeightmapSurface = true;
};

class TileMeshFrameEnsurer {
public:
    template <typename TerrainCacheKeyFn,
              typename IngestAvailabilityFn,
              typename FindUpsampleSourceFn,
              typename EnsureAncestorMeshFn,
              typename IsCompleteRenderableFn,
              typename MarkResourcesDirtyFn>
    static void ensure(
        const TileMeshFrameEnsureInput& input,
        TerrainCacheKeyFn&& terrainCacheKey,
        IngestAvailabilityFn&& ingestAvailability,
        FindUpsampleSourceFn&& findUpsampleSource,
        EnsureAncestorMeshFn&& ensureAncestorMesh,
        IsCompleteRenderableFn&& isCompleteRenderable,
        MarkResourcesDirtyFn&& markResourcesDirty) {
        const bool contentTerrainQuadtreeOwnsSurface =
            input.hasTerrainQuadtree &&
            !input.allowLegacyHeightmapSurface;
        const bool hasLegacySurfaceResidue =
            contentTerrainQuadtreeOwnsSurface &&
            (input.tile.content.renderContent.hasSurfaceMesh() ||
             input.tile.content.renderContent.hasRetainedHeightmap());
        if (hasLegacySurfaceResidue) {
            input.tile.content.renderContent.clearSurfaceMeshResources();
            input.tile.content.renderContent.clearRetainedHeightmap();
            markResourcesDirty();
        }

        if (input.tile.content.renderContent.hasGltfContent()) {
            return;
        }

        auto it = input.allowLegacyHeightmapSurface
            ? input.terrainCache.find(terrainCacheKey(input.tile.key))
            : input.terrainCache.end();
        const bool hasOwnTerrain =
            it != input.terrainCache.end() && it->second;
        DecodedHeightmap* ownHeightmap =
            hasOwnTerrain ? it->second.get() : nullptr;

        const TileSurfaceMeshEnsureResult result =
            TileSurfaceMeshEnsurer::ensure(
                TileSurfaceMeshEnsureInput{
                    input.tile,
                    ownHeightmap,
                    input.device,
                    input.hasTerrainQuadtree,
                    input.allowLegacyHeightmapSurface},
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
