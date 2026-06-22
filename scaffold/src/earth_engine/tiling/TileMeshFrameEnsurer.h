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
        if (input.tile.content.renderContent.hasGltfContent()) {
            return;
        }

        auto it = input.terrainCache.find(terrainCacheKey(input.tile.key));
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
                    input.hasTerrainQuadtree},
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
