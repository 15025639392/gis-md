#pragma once

#include "TileMeshLegacyHeightmapMode.h"
#include "TileSurfaceMeshResolutionPolicy.h"
#include "TileSurfaceMeshSourceResolver.h"
#include "TileSurfaceRenderContentCoordinator.h"
#include "TilesetTile.h"

#include "../providers/TerrainProvider.h"
#include "../terrain/TerrainTile.h"

namespace earth_engine {

struct TileSurfaceMeshEnsureInput {
    TilesetTile& tile;
    DecodedHeightmap* ownHeightmap = nullptr;
    RenderDevice* device = nullptr;
    bool hasTerrainQuadtree = false;
    bool allowEllipsoidFallbackWithoutTerrain = true;
    bool allowLegacySurfaceUpsample = true;
    TileMeshLegacyHeightmapMode legacyHeightmapMode =
        TileMeshLegacyHeightmapMode::Include;
};

struct TileSurfaceMeshEnsureResult {
    bool resourcesDirty = false;
};

class TileSurfaceMeshEnsurer {
public:
    static bool shouldReplaceReadySurface(const TilesetTile& tile,
                                          bool hasOwnTerrain) {
        return TileSurfaceMeshResolutionPolicy::shouldReplaceReadySurface(
            tile,
            hasOwnTerrain);
    }

    template <typename IngestAvailabilityFn,
              typename FindUpsampleSourceFn,
              typename EnsureAncestorMeshFn,
              typename IsCompleteRenderableFn>
    static TileSurfaceMeshEnsureResult ensure(
        const TileSurfaceMeshEnsureInput& input,
        IngestAvailabilityFn&& ingestAvailability,
        FindUpsampleSourceFn&& findUpsampleSource,
        EnsureAncestorMeshFn&& ensureAncestorMesh,
        IsCompleteRenderableFn&& isCompleteRenderable) {
        TilesetTile& tile = input.tile;
        DecodedHeightmap* ownHeightmap = input.ownHeightmap;
        const bool hasOwnTerrain = ownHeightmap != nullptr;

        if (tile.content.renderContent.hasGltfContent()) {
            return TileSurfaceMeshEnsureResult{};
        }

        if (tile.content.renderContent.isMeshReady()) {
            if (shouldReplaceReadySurface(tile, hasOwnTerrain)) {
                tile.content.renderContent.clearSurfaceMeshResources();
            } else {
                tile.refreshSurfaceDrawable(tile.hasSurfaceDrawable());
                if (tile.content.contentKind == TileContentKind::Render &&
                    tile.content.loadState == TileLoadState::ContentLoaded) {
                    tile.markRenderContentDone();
                }
                return TileSurfaceMeshEnsureResult{};
            }
        }

        if (ownHeightmap) {
            ingestAvailability(tile.key, ownHeightmap);
        }

        if (!hasOwnTerrain &&
            tile.content.derivesTerrainFromParent() &&
            TileGltfTerrainUpsampledChildMaterializer::findGltfTerrainSource(
                tile) != nullptr) {
            return TileSurfaceMeshEnsureResult{};
        }

        const bool contentOwnedTerrainOnly =
            input.legacyHeightmapMode ==
            TileMeshLegacyHeightmapMode::ContentOwnedTerrainOnly;
        TileSurfaceMeshResolution resolution =
            TileSurfaceMeshSourceResolver::resolve(
                tile,
                ownHeightmap,
                input.hasTerrainQuadtree,
                input.allowEllipsoidFallbackWithoutTerrain &&
                    !contentOwnedTerrainOnly,
                input.allowLegacySurfaceUpsample &&
                    !contentOwnedTerrainOnly,
                findUpsampleSource,
                ensureAncestorMesh);
        if (!tile.content.renderContent.hasSurfaceMesh()) {
            return TileSurfaceMeshEnsureResult{};
        }

        TileSurfaceRenderContentCoordinator::commitSurface(
            tile,
            TileSurfaceRenderContentCommit{
                resolution.resolvedSource(),
                resolution.markDone,
                ownHeightmap,
                input.device},
            isCompleteRenderable);

        return TileSurfaceMeshEnsureResult{true};
    }
};

} // namespace earth_engine
