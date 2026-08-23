#include <gtest/gtest.h>

#include "earth_engine/data/AmapGeometry.h"
#include "earth_engine/data/AmapVectorTile.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace earth_engine;

namespace {

constexpr double kRadToDeg = 180.0 / 3.14159265358979323846;

}  // namespace

TEST(AmapGeometryTest, CoordScalePerType) {
    EXPECT_DOUBLE_EQ(2.0, amapCoordScale(1, 14));
    EXPECT_DOUBLE_EQ(4.0, amapCoordScale(1, 10));
    EXPECT_DOUBLE_EQ(8.0, amapCoordScale(1, 3));
    EXPECT_DOUBLE_EQ(4.0, amapCoordScale(2, 14));
    EXPECT_DOUBLE_EQ(1.0 / 16.0, amapCoordScale(3, 14));
    EXPECT_DOUBLE_EQ(2.0, amapCoordScale(4, 14));
}

TEST(AmapGeometryTest, TileLocalToLngLatFlipY) {
    // z0 单瓦:flipY → 左上 (-180, 90)、右下 (180, -90)。
    Cartographic tl = amapTileLocalToLngLat(0, 0, 0, 0.0, 0.0);
    EXPECT_NEAR(-180.0, tl.longitude() * kRadToDeg, 1e-6);
    EXPECT_NEAR(90.0, tl.latitude() * kRadToDeg, 1e-6);
    Cartographic br = amapTileLocalToLngLat(0, 0, 0, 8192.0, 4096.0);
    EXPECT_NEAR(180.0, br.longitude() * kRadToDeg, 1e-6);
    EXPECT_NEAR(-90.0, br.latitude() * kRadToDeg, 1e-6);
}

TEST(AmapGeometryTest, DecodedPartToFeaturesLandsInChongqing) {
    AmapDecodedLayerPart part;
    part.z = 14;
    part.x = 13038;
    part.y = 5505;  // 高德 4326 网格:重庆 (106.5E, 29.5N)
    part.type = 1;
    AmapDecodedFeature f;
    f.classCode = 20009;
    f.geomType = 13;
    f.rings = {{{0, 0}, {2048, 0}, {2048, 1024}}};  // 原始 extent 4096×2048
    part.features.push_back(std::move(f));

    const auto features = amapDecodedPartToFeatures(part, /*toWgs84=*/true);
    ASSERT_EQ(1u, features.size());
    EXPECT_EQ(GeometryType::LineString, features[0].type);
    EXPECT_EQ("20009", features[0].properties.at("amap_class"));
    ASSERT_EQ(1u, features[0].rings.size());
    ASSERT_EQ(3u, features[0].rings[0].size());
    const double lon =
        features[0].rings[0][0].longitude() * kRadToDeg;
    const double lat = features[0].rings[0][0].latitude() * kRadToDeg;
    // 13038_5505_14 落在重庆附近(经度 ~106.4-106.5,纬度 ~29.4-29.5)。
    EXPECT_GT(lon, 105.0);
    EXPECT_LT(lon, 108.0);
    EXPECT_GT(lat, 28.0);
    EXPECT_LT(lat, 31.0);
}

TEST(AmapGeometryTest, GcjDeOffsetMovesInsideChina) {
    AmapDecodedLayerPart part;
    part.z = 14;
    part.x = 13038;
    part.y = 5505;
    part.type = 1;
    AmapDecodedFeature f;
    f.classCode = 20009;
    f.rings = {{{0, 0}}};
    part.features.push_back(std::move(f));

    const auto raw = amapDecodedPartToFeatures(part, /*toWgs84=*/false);
    const auto wgs = amapDecodedPartToFeatures(part, /*toWgs84=*/true);
    const double dLon =
        (wgs[0].rings[0][0].longitude() - raw[0].rings[0][0].longitude()) *
        kRadToDeg;
    const double dLat =
        (wgs[0].rings[0][0].latitude() - raw[0].rings[0][0].latitude()) *
        kRadToDeg;
    // 重庆境内 GCJ 偏移量级 ≈ 0.002-0.005°。
    EXPECT_GT(std::abs(dLon), 1e-4);
    EXPECT_GT(std::abs(dLat), 1e-4);
}

// 真样本端到端:AMAP_SAMPLE_TILE 指向真实瓦片时,解码→转换全部落在瓦片
// 地理范围内(重庆 ~106.4-106.5E / 29.4-29.5N)。
TEST(AmapGeometryTest, RealSampleConvertsToChongqingBounds) {
    const char* path = std::getenv("AMAP_SAMPLE_TILE");
    if (!path) GTEST_SKIP() << "AMAP_SAMPLE_TILE unset";
    FILE* f = std::fopen(path, "rb");
    ASSERT_NE(nullptr, f);
    std::fseek(f, 0, SEEK_END);
    const long len = std::ftell(f);
    std::rewind(f);
    std::vector<uint8_t> raw(static_cast<size_t>(len));
    ASSERT_EQ(len, static_cast<long>(std::fread(raw.data(), 1, raw.size(), f)));
    std::fclose(f);

    std::vector<AmapDecodedLayerPart> parts;
    ASSERT_TRUE(decodeAmapTile(raw.data(), raw.size(), parts));
    size_t total = 0;
    size_t buildings = 0;
    for (const auto& p : parts) {
        const auto feats = amapDecodedPartToFeatures(p);
        total += feats.size();
        if (p.type == 3) buildings += feats.size();
        for (const auto& feat : feats) {
            if (feat.properties.count("amap_height")) {
                EXPECT_GT(std::stod(feat.properties.at("amap_height")), 0.0);
            }
            for (const auto& ring : feat.rings) {
                for (const auto& c : ring) {
                    const double lon = c.longitude() * kRadToDeg;
                    const double lat = c.latitude() * kRadToDeg;
                    EXPECT_GT(lon, 104.0);
                    EXPECT_LT(lon, 109.0);
                    EXPECT_GT(lat, 27.0);
                    EXPECT_LT(lat, 32.0);
                }
            }
        }
    }
    EXPECT_GT(total, 0u);
    EXPECT_GT(buildings, 0u);  // 正确瓦片(13038_5505_14)含 type-3 建筑层
}
