#pragma once

#include <cstdint>

namespace earth_engine {

class TileContentCacheManager;
struct TilesetTile;

class TileContentResourceInvalidator {
public:
    TileContentResourceInvalidator(
        uint64_t& resourceRevision,
        TileContentCacheManager& contentCache);

    void markResourcesChanged();
    void reconcileTileResources(TilesetTile& tile);
    void markTileResourcesChanged(TilesetTile& tile);

private:
    uint64_t& resourceRevision_;
    TileContentCacheManager& contentCache_;
};

} // namespace earth_engine
