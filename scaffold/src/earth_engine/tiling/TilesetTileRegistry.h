#pragma once

#include "TilesetTile.h"
#include "../content/GltfContentProvider.h"

#include <memory>
#include <string>
#include <unordered_map>

namespace earth_engine {

class TileScheme;

class TilesetTileRegistry {
public:
    using TileMap =
        std::unordered_map<std::string, std::unique_ptr<TilesetTile>>;

    TilesetTile* ensureTile(
        const TileKey& key,
        const TileScheme& tileScheme,
        const TilesetContentProvider* contentProvider,
        size_t rasterOverlayCount);

    TilesetTile* findTile(const TileKey& key);
    const TilesetTile* findTile(const TileKey& key) const;

    TileMap& tiles() { return tiles_; }
    const TileMap& tiles() const { return tiles_; }

private:
    TileMap tiles_;
};

} // namespace earth_engine
