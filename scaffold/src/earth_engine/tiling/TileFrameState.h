#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace earth_engine {

struct TilesetTile;

struct TileFrameInactiveEntry {
    // Borrowed from the registry map's key — the map owns it and isn't mutated
    // between collection and consumption within a single frame's maintenance,
    // so no per-inactive-tile string copy is made.
    const std::string* cacheKey = nullptr;
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
