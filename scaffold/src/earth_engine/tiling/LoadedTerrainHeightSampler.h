#pragma once

#include <memory>
#include <string>
#include <unordered_map>

namespace earth_engine {

struct TilesetTile;

class LoadedTerrainHeightSampler {
public:
    static float sampleHeight(
        const std::unordered_map<
            std::string,
            std::unique_ptr<TilesetTile>>& tiles,
        double longitudeRadians,
        double latitudeRadians);
};

} // namespace earth_engine
