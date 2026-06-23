#pragma once

#include <memory>
#include <string>
#include <unordered_map>

namespace earth_engine {

struct DecodedHeightmap;
struct TilesetTile;

enum class LoadedTerrainHeightCacheMode {
    IncludeLegacyHeightmap,
    ContentOwnedTerrainOnly
};

class LoadedTerrainHeightSampler {
public:
    static float sampleHeight(
        const std::unordered_map<
            std::string,
            std::unique_ptr<TilesetTile>>& tiles,
        const std::unordered_map<
            std::string,
            std::unique_ptr<DecodedHeightmap>>& terrainCache,
        double longitudeRadians,
        double latitudeRadians,
        LoadedTerrainHeightCacheMode cacheMode =
            LoadedTerrainHeightCacheMode::IncludeLegacyHeightmap);
};

} // namespace earth_engine
