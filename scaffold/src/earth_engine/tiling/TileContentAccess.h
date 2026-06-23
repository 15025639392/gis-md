#pragma once

#include "TileKey.h"
#include "TileContentLifecycleManager.h"
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
        const TerrainProvider* heightmapTerrainProvider,
        const TilesetContentProvider* contentProvider,
        const HeightmapTerrainCache& heightmapTerrainCache,
        size_t rasterOverlayCount);

    TilesetTile* ensureTile(const TileKey& key);
    void ensureTileChildren(TilesetTile& tile);
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
                      const TerrainProvider* heightmapTerrainProvider,
                      const TilesetContentProvider* contentProvider,
                      const HeightmapTerrainCache* heightmapTerrainCache,
                      TerrainOwnership terrainOwnership,
                      size_t rasterOverlayCount);

    bool contentProviderOwnsTerrainQuadtree() const;
    bool hasTerrainQuadtree() const;
    bool heightmapAvailabilityBoundaryTile(const TilesetTile& tile) const;
    bool contentTerrainAvailabilityBoundaryTile(const TilesetTile& tile) const;
    TileAvailabilityState availabilityState(const TileKey& key) const;
    TileAvailabilityState heightmapAvailabilityState(const TileKey& key) const;
    TileAvailabilityState contentTerrainAvailabilityState(
        const TileKey& key) const;

    TilesetTileRegistry& tileRegistry_;
    const TileScheme& tileScheme_;
    const TerrainProvider* heightmapTerrainProvider_ = nullptr;
    const TilesetContentProvider* contentProvider_ = nullptr;
    const HeightmapTerrainCache* heightmapTerrainCache_ = nullptr;
    TerrainOwnership terrainOwnership_ = TerrainOwnership::None;
    size_t rasterOverlayCount_ = 0;
};

} // namespace earth_engine
