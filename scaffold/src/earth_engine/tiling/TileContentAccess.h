#pragma once

#include "TileKey.h"
#include "TileChildFrameMaterializer.h"
#include "TileContentLifecycleManager.h"
#include "TileLegacyHeightmapContentResolver.h"
#include "RasterMappedToTilesetTile.h"
#include "TilesetTile.h"

#include <cstddef>

namespace earth_engine {

class TerrainProvider;
class TileContentLifecycleManager;
class TileScheme;
class TilesetContentProvider;
class TilesetTileRegistry;

class TileContentAccess {
public:
    static TileContentAccess forContentTerrain(
        TilesetTileRegistry& tileRegistry,
        const TileScheme& tileScheme,
        const TilesetContentProvider& contentProvider,
        size_t rasterOverlayCount);

    static TileContentAccess forNoTerrain(
        TilesetTileRegistry& tileRegistry,
        const TileScheme& tileScheme,
        const TilesetContentProvider* contentProvider,
        size_t rasterOverlayCount);

    static TileContentAccess forHeightmapTerrainSurfacePath(
        TilesetTileRegistry& tileRegistry,
        const TileScheme& tileScheme,
        const TerrainProvider* legacyHeightmapTerrainProvider,
        const TilesetContentProvider* contentProvider,
        const LegacyHeightmapTerrainCache& legacyHeightmapTerrainCache,
        size_t rasterOverlayCount);

    TilesetTile* ensureTile(const TileKey& key);
    TileChildFrameMaterializeResult ensureTileChildren(TilesetTile& tile);
    bool hasResolvedAvailabilityBoundaryContent(const TilesetTile& tile) const;
    bool isAvailabilityBoundaryTile(const TilesetTile& tile) const;
    bool canRefine(const TilesetTile& tile) const;

private:
    enum class TerrainOwnership {
        None,
        HeightmapSurface,
        ContentProvider,
    };

    TileContentAccess(TilesetTileRegistry& tileRegistry,
                      const TileScheme& tileScheme,
                      const TerrainProvider* legacyHeightmapTerrainProvider,
                      const TilesetContentProvider* contentProvider,
                      const LegacyHeightmapTerrainCache* legacyHeightmapTerrainCache,
                      TerrainOwnership terrainOwnership,
                      size_t rasterOverlayCount);

    bool contentProviderOwnsTerrainQuadtree() const;
    bool hasTerrainQuadtree() const;
    bool contentTerrainAvailabilityBoundaryTile(const TilesetTile& tile) const;
    TileAvailabilityState availabilityState(const TileKey& key) const;
    TileAvailabilityState contentTerrainAvailabilityState(
        const TileKey& key) const;

    TilesetTileRegistry& tileRegistry_;
    const TileScheme& tileScheme_;
    const TilesetContentProvider* contentProvider_ = nullptr;
    TerrainOwnership terrainOwnership_ = TerrainOwnership::None;
    TileLegacyHeightmapContentResolver legacyHeightmapContent_;
    size_t rasterOverlayCount_ = 0;
};

} // namespace earth_engine
