#include "LoadedTerrainHeightSampler.h"

#include "DecodedHeightmapSampler.h"
#include "RasterMappedToTilesetTile.h"
#include "TilesetTile.h"

#include "../providers/TerrainProvider.h"

namespace earth_engine {

float LoadedTerrainHeightSampler::sampleHeight(
    const std::unordered_map<
        std::string,
        std::unique_ptr<TilesetTile>>& tiles,
    const std::unordered_map<
        std::string,
        std::unique_ptr<DecodedHeightmap>>& terrainCache,
    double longitudeRadians,
    double latitudeRadians) {
    const DecodedHeightmap* bestHeightmap = nullptr;
    Rectangle bestBounds;
    int bestZoom = -1;

    for (const auto& [cacheKey, tile] : tiles) {
        if (!tile || tile->key.z < bestZoom ||
            !tile->bounds.contains(longitudeRadians, latitudeRadians)) {
            continue;
        }
        auto terrainIt = terrainCache.find(cacheKey);
        if (terrainIt == terrainCache.end() || !terrainIt->second ||
            !terrainIt->second->valid()) {
            continue;
        }

        bestHeightmap = terrainIt->second.get();
        bestBounds = tile->bounds;
        bestZoom = tile->key.z;
    }

    if (!bestHeightmap) {
        return 0.0f;
    }
    return DecodedHeightmapSampler::sampleHeight(
        *bestHeightmap,
        bestBounds,
        longitudeRadians,
        latitudeRadians);
}

} // namespace earth_engine
