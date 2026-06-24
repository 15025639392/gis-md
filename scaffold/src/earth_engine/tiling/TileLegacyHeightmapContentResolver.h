#pragma once

#include "TileContentLifecycleManager.h"
#include "TileKey.h"

namespace earth_engine {

class TerrainProvider;
class TileScheme;
class TilesetContentProvider;
struct TilesetTile;

class TileLegacyHeightmapContentResolver {
public:
    TileLegacyHeightmapContentResolver(
        const TerrainProvider* terrainProvider,
        const TilesetContentProvider* contentProvider,
        const TileScheme& tileScheme,
        const LegacyHeightmapTerrainCache* terrainCache);

    bool isAvailabilityBoundaryTile(const TilesetTile& tile) const;
    bool canRefine(const TilesetTile& tile) const;
    TileAvailabilityState availabilityState(const TileKey& key) const;

private:
    const TerrainProvider* terrainProvider_ = nullptr;
    const TilesetContentProvider* contentProvider_ = nullptr;
    const TileScheme* tileScheme_ = nullptr;
};

} // namespace earth_engine
