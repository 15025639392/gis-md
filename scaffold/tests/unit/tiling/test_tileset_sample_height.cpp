#include <gtest/gtest.h>

#include "earth_engine/tiling/TileCacheKey.h"
#include "earth_engine/tiling/TileScheme.h"
#include "earth_engine/tiling/Tileset.h"

#include <memory>

using namespace earth_engine;

namespace earth_engine {
struct TilesetTestAccess {
    static TilesetTile* ensureTile(Tileset& tileset, const TileKey& key) {
        return tileset.contentAccess_.ensureTile(key);
    }

    static void putTerrainCache(
        Tileset& tileset,
        const TileKey& key,
        std::unique_ptr<DecodedHeightmap> heightmap) {
        tileset.contentLifecycle_.terrainCache()[TileCacheKey::forTile(key)] =
            std::move(heightmap);
    }
};
} // namespace earth_engine

namespace {

std::unique_ptr<DecodedHeightmap> makeFlatHeightmap(float heightMeters) {
    auto heightmap = std::make_unique<DecodedHeightmap>();
    heightmap->tileSize = 2;
    heightmap->heights = {
        heightMeters,
        heightMeters,
        heightMeters,
        heightMeters};
    heightmap->minHeight = heightMeters;
    heightmap->maxHeight = heightMeters;
    return heightmap;
}

std::pair<double, double> tileCenter(
    const TileScheme& scheme,
    const TileKey& key) {
    const Rectangle bounds = scheme.tileToRectangle(key);
    return {
        (bounds.west() + bounds.east()) * 0.5,
        (bounds.south() + bounds.north()) * 0.5};
}

Tileset makeHeightSamplingTileset() {
    return Tileset(
        std::unique_ptr<TerrainProvider>{},
        TileScheme::createGeographicTMS(),
        {},
        nullptr,
        TilesetOptions{});
}

} // namespace

TEST(TilesetSampleHeightTest, UsesMostDetailedLoadedTerrainTile) {
    Tileset tileset = makeHeightSamplingTileset();
    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    const TileKey childKey{"Geographic-TMS", 1, 0, 0};

    TilesetTestAccess::ensureTile(tileset, rootKey);
    TilesetTestAccess::ensureTile(tileset, childKey);
    TilesetTestAccess::putTerrainCache(
        tileset,
        rootKey,
        makeFlatHeightmap(10.0f));
    TilesetTestAccess::putTerrainCache(
        tileset,
        childKey,
        makeFlatHeightmap(42.0f));

    const auto [longitude, latitude] =
        tileCenter(tileset.tileScheme(), childKey);

    EXPECT_NEAR(tileset.sampleHeight(longitude, latitude), 42.0f, 1e-6f);
}

TEST(TilesetSampleHeightTest, FallsBackToLoadedAncestorTerrain) {
    Tileset tileset = makeHeightSamplingTileset();
    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    const TileKey childKey{"Geographic-TMS", 1, 0, 0};

    TilesetTestAccess::ensureTile(tileset, rootKey);
    TilesetTestAccess::ensureTile(tileset, childKey);
    TilesetTestAccess::putTerrainCache(
        tileset,
        rootKey,
        makeFlatHeightmap(123.0f));

    const auto [longitude, latitude] =
        tileCenter(tileset.tileScheme(), childKey);

    EXPECT_NEAR(tileset.sampleHeight(longitude, latitude), 123.0f, 1e-6f);
}
