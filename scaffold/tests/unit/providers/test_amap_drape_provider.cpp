#include <gtest/gtest.h>

#include "earth_engine/providers/AmapDrapeImageryProvider.h"
#include "earth_engine/providers/MvtRectCoverage.h"

#include <cmath>
#include <memory>
#include <vector>

using namespace earth_engine;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg = kPi / 180.0;

AmapDrapeImageryProvider::Options defaultOptions() {
    AmapDrapeImageryProvider::Options opt;
    opt.tileSize = 16;
    opt.advertisedMaxZoom = 18;
    opt.dataMinZoom = 10;
    opt.dataMaxZoom = 10;
    opt.maxSourceTiles = 16;
    VectorRasterLayerPaint paint;
    paint.layer = "*";
    paint.fillColor = {255, 0, 0, 255};
    opt.style.layers = {paint};
    opt.style.supersample = 1;
    return opt;
}

std::shared_ptr<AmapDrapeImageryProvider::RegionCache> emptyCache() {
    return std::make_shared<AmapDrapeImageryProvider::RegionCache>(
        [](const TileKey&,
           AmapDrapeImageryProvider::RegionCache::FetchCallback cb) {
            cb(404, {});
        },
        8);
}

std::unique_ptr<DecodedImage> request(AmapDrapeImageryProvider& provider,
                                      const TileKey& key) {
    std::unique_ptr<DecodedImage> got;
    provider.requestTile(key, CancellationToken(),
                         [&](const TileKey&,
                             std::unique_ptr<DecodedImage> img) {
                             got = std::move(img);
                         });
    return got;
}

}  // namespace

TEST(AmapDrapeCoverageTest, PointInChongqingHitsOneZ10Tile) {
    const double lng = 106.508 * kDeg;
    const double lat = 29.617 * kDeg;
    const auto tiles = mvt_rect::amapGeographicCoverage(
        lng, lat, lng, lat, 10);
    ASSERT_EQ(tiles.size(), 1u);
    // 与 TileScheme::createAmapGeographic 同公式交叉检查。
    const int n = 1024;
    const int expectX =
        static_cast<int>(std::floor((106.508 + 180.0) / 360.0 * n));
    const int expectY =
        static_cast<int>(std::floor((90.0 - 29.617) / 180.0 * n));
    EXPECT_EQ(tiles[0].x, expectX);
    EXPECT_EQ(tiles[0].y, expectY);
}

TEST(AmapDrapeProviderTest, TooManySourceTilesYieldsTransparentImage) {
    auto opt = defaultOptions();
    opt.maxSourceTiles = 1;
    AmapDrapeImageryProvider provider(opt, emptyCache());
    // z0 整世界页,z10 覆盖 1024 瓦,超过上限 → 透明图,不发网络。
    auto got = request(provider, TileKey{"XYZ-WebMercator", 0, 0, 0});
    ASSERT_NE(got, nullptr);
    EXPECT_EQ(got->width, 16);
    bool any = false;
    for (size_t i = 3; i < got->pixels.size(); i += 4) {
        if (got->pixels[i] != 0) any = true;
    }
    EXPECT_FALSE(any);
}

TEST(AmapDrapeProviderTest, FetchFailureYieldsTransparentImageNotNull) {
    AmapDrapeImageryProvider provider(defaultOptions(), emptyCache());
    // 重庆附近 z14 页,z10 覆盖 1-4 瓦,fetch 404 → 透明图。
    auto got = request(provider, TileKey{"XYZ-WebMercator", 14, 12850, 7130});
    ASSERT_NE(got, nullptr) << "失败必须降级成透明图,不能卡死页合成";
    bool any = false;
    for (size_t i = 3; i < got->pixels.size(); i += 4) {
        if (got->pixels[i] != 0) any = true;
    }
    EXPECT_FALSE(any);
}

TEST(AmapDrapeProviderTest, CancelledRequestStillInvokesCallback) {
    AmapDrapeImageryProvider provider(defaultOptions(), emptyCache());
    CancellationToken token;
    token.cancel();
    bool called = false;
    std::unique_ptr<DecodedImage> got;
    provider.requestTile(TileKey{"XYZ-WebMercator", 14, 12850, 7130}, token,
                         [&](const TileKey&,
                             std::unique_ptr<DecodedImage> img) {
                             called = true;
                             got = std::move(img);
                         });
    EXPECT_TRUE(called);
    EXPECT_EQ(got, nullptr);
}

TEST(AmapDrapeProviderTest, SetStyleAdvancesContentRevision) {
    AmapDrapeImageryProvider provider(defaultOptions(), emptyCache());
    const uint64_t before = provider.contentRevision();
    VectorRasterStyle style;
    provider.setStyle(std::move(style));
    EXPECT_GT(provider.contentRevision(), before);
}
