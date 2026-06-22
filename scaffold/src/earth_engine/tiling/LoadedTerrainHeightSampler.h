#pragma once

#include <memory>
#include <string>
#include <unordered_map>

namespace earth_engine {

struct DecodedHeightmap;
struct TilesetTile;

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
        bool useTerrainCache = true);
};

} // namespace earth_engine
