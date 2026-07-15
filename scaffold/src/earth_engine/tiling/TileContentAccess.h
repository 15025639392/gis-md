#pragma once

#include "TileKey.h"
#include "TileChildFrameMaterializer.h"
#include "TileContentLifecycleManager.h"
#include "RasterMappedToTilesetTile.h"
#include "TilesetTile.h"

namespace earth_engine {

class TileContentLifecycleManager;
class TileContentResourceInvalidator;
class IPrepareRendererResources;
class TileScheme;
class TilesetContentProvider;
class TilesetTileRegistry;

class TileContentAccess {
public:
    static TileContentAccess forContentTerrain(
        TilesetTileRegistry& tileRegistry,
        const TileScheme& tileScheme,
        const TilesetContentProvider& contentProvider,
        TileContentResourceInvalidator* resourceInvalidator = nullptr);

    static TileContentAccess forNoTerrain(
        TilesetTileRegistry& tileRegistry,
        const TileScheme& tileScheme,
        const TilesetContentProvider* contentProvider,
        TileContentResourceInvalidator* resourceInvalidator = nullptr);

    TilesetTile* ensureTile(const TileKey& key,
                            IPrepareRendererResources* pPrepRenderer = nullptr);
    TileChildFrameMaterializeResult ensureTileChildren(
        TilesetTile& tile,
        IPrepareRendererResources* pPrepRenderer = nullptr);
    bool hasResolvedAvailabilityBoundaryContent(const TilesetTile& tile) const;
    bool isAvailabilityBoundaryTile(const TilesetTile& tile) const;
    bool canRefine(const TilesetTile& tile) const;

private:
    TileContentAccess(TilesetTileRegistry& tileRegistry,
                      const TileScheme& tileScheme,
                      const TilesetContentProvider* contentProvider,
                      bool contentProviderOwnsTerrainQuadtree,
                      TileContentResourceInvalidator* resourceInvalidator);

    bool contentProviderOwnsTerrainQuadtree() const;
    bool hasTerrainQuadtree() const;
    bool contentTerrainAvailabilityBoundaryTile(const TilesetTile& tile) const;
    TileAvailabilityState availabilityState(const TileKey& key) const;
    TileAvailabilityState contentTerrainAvailabilityState(
        const TileKey& key) const;

    TilesetTileRegistry& tileRegistry_;
    const TileScheme& tileScheme_;
    const TilesetContentProvider* contentProvider_ = nullptr;
    bool contentProviderOwnsTerrainQuadtree_ = false;
    TileContentResourceInvalidator* resourceInvalidator_ = nullptr;
};

} // namespace earth_engine
