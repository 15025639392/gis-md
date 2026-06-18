#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

namespace earth_engine {

struct DecodedHeightmap;
struct TilesetTile;

struct TileCacheMetrics {
    static int64_t estimateHeightmapBytes(const DecodedHeightmap& heightmap);
    static int64_t estimateTileBytes(const TilesetTile& tile);
    static int64_t estimateTotalBytes(
        const std::unordered_map<
            std::string,
            std::unique_ptr<TilesetTile>>& tiles,
        const std::unordered_map<
            std::string,
            std::unique_ptr<DecodedHeightmap>>& terrainCache);
};

} // namespace earth_engine
