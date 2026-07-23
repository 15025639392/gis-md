#include <gtest/gtest.h>

#include "earth_engine/providers/TerrainProvider.h"
#include "earth_engine/tiling/DecodedHeightmapSampler.h"
#include "earth_engine/tiling/TileScheme.h"

using namespace earth_engine;

namespace {

// A geographic rectangle to serve as sourceBounds; any rectangle works since
// the sampler maps lon/lat to [0,1] tile coordinates relative to it.
Rectangle rootBounds() {
    auto scheme = TileScheme::createGeographicTMS();
    return scheme->tileToRectangle(TileKey{"Geographic-TMS", 0, 0, 0});
}

} // namespace

TEST(DecodedHeightmapSamplerTest, SamplesUniformHeight) {
    DecodedHeightmap heightmap;
    heightmap.tileSize = 2;
    heightmap.heights = {42.0f, 42.0f, 42.0f, 42.0f};
    const Rectangle bounds = rootBounds();

    const double lon = bounds.west() + bounds.width() * 0.5;
    const double lat = bounds.south() + bounds.height() * 0.5;
    EXPECT_NEAR(42.0f,
                DecodedHeightmapSampler::sampleHeight(heightmap, bounds, lon, lat),
                1e-4f);
}

TEST(DecodedHeightmapSamplerTest, BilinearInterpolatesGradient) {
    // Row-major, north→south rows / west→east columns:
    //   row0 (north): NW=30, NE=40
    //   row1 (south): SW=10, SE=20
    // At u=0.25 (from west), 0.25 from south (→ v=0.75 north→south), the
    // bilinear blend is 17.5.
    DecodedHeightmap heightmap;
    heightmap.tileSize = 2;
    heightmap.heights = {30.0f, 40.0f, 10.0f, 20.0f};
    const Rectangle bounds = rootBounds();

    const double lon = bounds.west() + bounds.width() * 0.25;
    const double lat = bounds.south() + bounds.height() * 0.25;
    EXPECT_NEAR(17.5f,
                DecodedHeightmapSampler::sampleHeight(heightmap, bounds, lon, lat),
                1e-4f);
}

TEST(DecodedHeightmapSamplerTest, ReturnsZeroOutsideBounds) {
    DecodedHeightmap heightmap;
    heightmap.tileSize = 2;
    heightmap.heights = {50.0f, 50.0f, 50.0f, 50.0f};
    const Rectangle bounds = rootBounds();

    // A longitude well east of the rectangle's east edge → out of bounds → 0.
    const double lon = bounds.east() + bounds.width();
    const double lat = bounds.south() + bounds.height() * 0.5;
    EXPECT_NEAR(0.0f,
                DecodedHeightmapSampler::sampleHeight(heightmap, bounds, lon, lat),
                1e-4f);
}

TEST(DecodedHeightmapSamplerTest, ReturnsZeroForNoData) {
    // All corners are a no-data sentinel (>50000) → sampler reports 0 (sea
    // level) rather than a spurious huge height.
    DecodedHeightmap heightmap;
    heightmap.tileSize = 2;
    heightmap.heights = {65535.0f, 65535.0f, 65535.0f, 65535.0f};
    const Rectangle bounds = rootBounds();

    const double lon = bounds.west() + bounds.width() * 0.5;
    const double lat = bounds.south() + bounds.height() * 0.5;
    EXPECT_NEAR(0.0f,
                DecodedHeightmapSampler::sampleHeight(heightmap, bounds, lon, lat),
                1e-4f);
}

TEST(DecodedHeightmapSamplerTest, ReturnsZeroForInvalidHeightmap) {
    DecodedHeightmap heightmap;  // tileSize == 0 → invalid.
    const Rectangle bounds = rootBounds();

    const double lon = bounds.west() + bounds.width() * 0.5;
    const double lat = bounds.south() + bounds.height() * 0.5;
    EXPECT_NEAR(0.0f,
                DecodedHeightmapSampler::sampleHeight(heightmap, bounds, lon, lat),
                1e-4f);
}
