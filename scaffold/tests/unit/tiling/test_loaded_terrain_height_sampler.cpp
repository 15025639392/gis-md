#include <gtest/gtest.h>

#include "earth_engine/providers/TerrainProvider.h"
#include "earth_engine/tiling/LoadedTerrainHeightSampler.h"
#include "earth_engine/tiling/RasterMappedToTilesetTile.h"
#include "earth_engine/tiling/TileScheme.h"
#include "earth_engine/tiling/TilesetTile.h"

#include <memory>
#include <string>
#include <unordered_map>

using namespace earth_engine;

namespace {

std::string cacheKeyFor(const TileKey& key) {
    return key.schemeId + ":" +
           std::to_string(key.z) + ":" +
           std::to_string(key.x) + ":" +
           std::to_string(key.y);
}

std::unique_ptr<DecodedHeightmap> makeFlatHeightmap(float height) {
    auto heightmap = std::make_unique<DecodedHeightmap>();
    heightmap->tileSize = 2;
    heightmap->heights.assign(4, height);
    heightmap->minHeight = height;
    heightmap->maxHeight = height;
    return heightmap;
}

void putTile(
    std::unordered_map<std::string, std::unique_ptr<TilesetTile>>& tiles,
    const TileKey& key,
    const Rectangle& bounds) {
    tiles.emplace(cacheKeyFor(key), std::make_unique<TilesetTile>(key, bounds));
}

} // namespace

TEST(LoadedTerrainHeightSamplerTest, UsesBestLoadedTerrainTile) {
    auto scheme = TileScheme::createGeographicTMS();
    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;
    std::unordered_map<std::string, std::unique_ptr<DecodedHeightmap>>
        terrainCache;

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    const TileKey childKey{"Geographic-TMS", 1, 0, 0};
    putTile(tiles, rootKey, scheme->tileToRectangle(rootKey));
    putTile(tiles, childKey, scheme->tileToRectangle(childKey));
    terrainCache.emplace(cacheKeyFor(rootKey), makeFlatHeightmap(10.0f));
    terrainCache.emplace(cacheKeyFor(childKey), makeFlatHeightmap(42.0f));

    const Rectangle childBounds = scheme->tileToRectangle(childKey);
    const double longitude = (childBounds.west() + childBounds.east()) * 0.5;
    const double latitude = (childBounds.south() + childBounds.north()) * 0.5;

    EXPECT_NEAR(
        42.0f,
        LoadedTerrainHeightSampler::sampleHeight(
            tiles,
            terrainCache,
            longitude,
            latitude),
        1e-6f);
}
