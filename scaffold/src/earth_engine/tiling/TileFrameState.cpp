#include "TileFrameState.h"

#include "DirectRasterMapping.h"
#include "TilesetTile.h"

namespace earth_engine {

std::vector<TileFrameInactiveEntry> TileFrameState::collectInactiveTiles(
    const std::unordered_map<std::string, std::unique_ptr<TilesetTile>>& tiles,
    uint64_t frameNumber) {
    std::vector<TileFrameInactiveEntry> result;
    result.reserve(tiles.size());
    for (const auto& [cacheKey, tile] : tiles) {
        if (!tile || tile->lastUsedFrame() == frameNumber) {
            continue;
        }
        result.push_back(TileFrameInactiveEntry{&cacheKey, tile.get()});
    }
    return result;
}

} // namespace earth_engine
