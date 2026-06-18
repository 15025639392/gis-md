#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace earth_engine {

struct TilesetTile;

struct TileFrameInactiveEntry {
    std::string cacheKey;
    TilesetTile* tile = nullptr;
};

struct TileFrameState {
    static std::vector<TileFrameInactiveEntry> collectInactiveTiles(
        const std::unordered_map<
            std::string,
            std::unique_ptr<TilesetTile>>& tiles,
        uint64_t frameNumber);
};

} // namespace earth_engine
