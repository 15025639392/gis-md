#include <gtest/gtest.h>

#include "earth_engine/tiling/RasterMappedToTilesetTile.h"
#include "earth_engine/tiling/TileCacheMetrics.h"
#include "earth_engine/tiling/TilesetTile.h"

#include <cstdint>
#include <memory>
#include <unordered_map>

using namespace earth_engine;

TEST(TileCacheMetricsTest, EstimateTileBytesUsesActualMeshPayload) {
    TilesetTile tile;
    tile.content.renderContent.setSurfaceMesh(std::make_unique<SurfaceTileMesh>());
    SurfaceTileMesh* mesh = tile.content.renderContent.mutableSurfaceMesh();
    ASSERT_NE(nullptr, mesh);

    mesh->vertices.resize(3);
    mesh->indices.resize(6);
    mesh->waterMask.data.resize(8);
    mesh->metadataAvailability.resize(2);

    const int64_t expected =
        static_cast<int64_t>(mesh->vertices.size() * sizeof(SurfaceVertex)) +
        static_cast<int64_t>(mesh->indices.size() * sizeof(uint32_t)) +
        static_cast<int64_t>(mesh->waterMask.data.size()) +
        static_cast<int64_t>(
            mesh->metadataAvailability.size() *
            sizeof(QuantizedMeshAvailabilityRange));

    EXPECT_EQ(expected, TileCacheMetrics::estimateTileBytes(tile));
}

TEST(TileCacheMetricsTest, CountsHeightmapAndRetainedTilePayloads) {
    DecodedHeightmap heightmap;
    heightmap.rawData.resize(7);
    heightmap.heights.resize(3);
    heightmap.noDataValues.resize(2);
    heightmap.metadataAvailability.resize(4);

    const int64_t expectedHeightmapBytes =
        7 +
        static_cast<int64_t>(3 * sizeof(float)) +
        static_cast<int64_t>(2 * sizeof(float)) +
        static_cast<int64_t>(
            4 * sizeof(QuantizedMeshAvailabilityRange));
    EXPECT_EQ(expectedHeightmapBytes,
              TileCacheMetrics::estimateHeightmapBytes(heightmap));

    TilesetTile tile;
    tile.key = TileKey{"test", 0, 0, 0};
    tile.content.renderContent.setSurfaceMesh(std::make_unique<SurfaceTileMesh>());
    SurfaceTileMesh* mesh = tile.content.renderContent.mutableSurfaceMesh();
    ASSERT_NE(nullptr, mesh);
    mesh->vertices.resize(2);
    mesh->indices.resize(5);
    mesh->waterMask.data.resize(6);
    mesh->metadataAvailability.resize(1);

    auto retainedHeightmap = std::make_unique<DecodedHeightmap>();
    retainedHeightmap->rawData.resize(7);
    retainedHeightmap->heights.resize(3);
    retainedHeightmap->noDataValues.resize(2);
    retainedHeightmap->metadataAvailability.resize(4);
    tile.content.renderContent.setRetainedHeightmap(std::move(retainedHeightmap));

    const int64_t expectedTileBytes =
        static_cast<int64_t>(2 * sizeof(SurfaceVertex)) +
        static_cast<int64_t>(5 * sizeof(uint32_t)) +
        6 +
        static_cast<int64_t>(sizeof(QuantizedMeshAvailabilityRange)) +
        expectedHeightmapBytes;
    EXPECT_EQ(expectedTileBytes, TileCacheMetrics::estimateTileBytes(tile));
}

TEST(TileCacheMetricsTest, TotalsTileAndTerrainCachePayloads) {
    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;
    auto tile = std::make_unique<TilesetTile>(
        TileKey{"test", 0, 0, 0},
        Rectangle{});
    tile->content.renderContent.setSurfaceMesh(std::make_unique<SurfaceTileMesh>());
    tile->content.renderContent.mutableSurfaceMesh()->vertices.resize(1);
    const int64_t expectedTileBytes =
        static_cast<int64_t>(sizeof(SurfaceVertex));
    tiles["tile"] = std::move(tile);
    tiles["null-tile"] = nullptr;

    std::unordered_map<std::string, std::unique_ptr<DecodedHeightmap>>
        terrainCache;
    auto heightmap = std::make_unique<DecodedHeightmap>();
    heightmap->rawData.resize(9);
    const int64_t expectedHeightmapBytes = 9;
    terrainCache["terrain"] = std::move(heightmap);
    terrainCache["null-terrain"] = nullptr;

    EXPECT_EQ(
        expectedTileBytes + expectedHeightmapBytes,
        TileCacheMetrics::estimateTotalBytes(tiles, terrainCache));
}
