#include <gtest/gtest.h>

#include "earth_engine/tiling/RasterMappedToTilesetTile.h"
#include "earth_engine/tiling/TileCacheMetrics.h"
#include "earth_engine/tiling/TilesetTile.h"

#include <cstdint>
#include <memory>

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
