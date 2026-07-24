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

// borderInset=0.5 契约:cell-registered + 1px 重叠环源(Mapbox 514)相邻瓦片
// 无缝。用世界线性高程 h(x)=x 构造两片相邻瓦片(tileSize=4 = 2 个内部 cell +
// 每侧 1px 重叠),验证东片西边界采样 == 西片东边界采样(共享边界 x=2.0)。
// 每行相同,退化成 1D 便于推理。
TEST(DecodedHeightmapSamplerTest, BorderInsetHalfPixelSeamless) {
    // 西片覆盖世界 [0,2]:内部 cell 中心 x=0.5,1.5;重叠列 x=-0.5(西邻),2.5(东邻)
    DecodedHeightmap west;
    west.tileSize = 4;
    west.borderInset = 0.5f;
    west.heights.clear();
    for (int r = 0; r < 4; ++r)
        for (float x : {-0.5f, 0.5f, 1.5f, 2.5f}) west.heights.push_back(x);
    // 东片覆盖世界 [2,4]:内部 cell 中心 x=2.5,3.5;重叠列 x=1.5(西邻),4.5(东邻)
    DecodedHeightmap east;
    east.tileSize = 4;
    east.borderInset = 0.5f;
    east.heights.clear();
    for (int r = 0; r < 4; ++r)
        for (float x : {1.5f, 2.5f, 3.5f, 4.5f}) east.heights.push_back(x);

    const float westEast = west.sampleBilinear(1.0f, 0.5f);  // 西片东边界 → x=2.0
    const float eastWest = east.sampleBilinear(0.0f, 0.5f);  // 东片西边界 → x=2.0
    EXPECT_NEAR(2.0f, westEast, 1e-4f);
    EXPECT_NEAR(2.0f, eastWest, 1e-4f);
    EXPECT_NEAR(westEast, eastWest, 1e-5f);  // 无缝:共享边界值完全一致
}

// borderInset=0(默认,顶点栅格源如自产 grid65)保持原映射 [0, tileSize-1]:
// u=0 落在像素 0、u=1 落在最后一像素,不内缩。
TEST(DecodedHeightmapSamplerTest, BorderInsetZeroPreservesVertexGrid) {
    DecodedHeightmap hm;
    hm.tileSize = 3;
    hm.borderInset = 0.0f;
    hm.heights = {0, 10, 20, 0, 10, 20, 0, 10, 20};  // 每行 0,10,20
    EXPECT_NEAR(0.0f, hm.sampleBilinear(0.0f, 0.5f), 1e-4f);   // 像素 0
    EXPECT_NEAR(20.0f, hm.sampleBilinear(1.0f, 0.5f), 1e-4f);  // 像素 2
    EXPECT_NEAR(10.0f, hm.sampleBilinear(0.5f, 0.5f), 1e-4f);  // 像素 1
}
