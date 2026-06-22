#pragma once

#include "TileKey.h"
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
    TileContentAccess(TilesetTileRegistry& tileRegistry,
                      const TileScheme& tileScheme,
                      const TerrainProvider* terrainProvider,
                      const TilesetContentProvider* contentProvider,
                      const TileContentLifecycleManager& contentLifecycle,
                      size_t rasterOverlayCount);

    TilesetTile* ensureTile(const TileKey& key);
    void ensureTileChildren(TilesetTile& tile);
    bool hasResolvedAvailabilityBoundaryContent(const TilesetTile& tile) const;
    bool isAvailabilityBoundaryTile(const TilesetTile& tile) const;
    bool canRefine(const TilesetTile& tile) const;

private:
    bool hasTerrainQuadtree() const;
    TileAvailabilityState availabilityState(const TileKey& key) const;

    TilesetTileRegistry& tileRegistry_;
    const TileScheme& tileScheme_;
    const TerrainProvider* terrainProvider_ = nullptr;
    const TilesetContentProvider* contentProvider_ = nullptr;
    const TileContentLifecycleManager& contentLifecycle_;
    size_t rasterOverlayCount_ = 0;
};

} // namespace earth_engine
