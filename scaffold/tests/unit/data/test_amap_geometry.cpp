#include <gtest/gtest.h>

#include "earth_engine/data/AmapGeometry.h"
#include "earth_engine/data/AmapVectorTile.h"
#include "earth_engine/data/PolygonTessellator.h"
#include "earth_engine/core/geodesy/Ellipsoid.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace earth_engine;

namespace {

constexpr double kRadToDeg = 180.0 / 3.14159265358979323846;

}  // namespace

TEST(AmapGeometryTest, CoordScalePerType) {
    EXPECT_EQ(2.0, amapCoordScale(1, 14));
    EXPECT_EQ(4.0, amapCoordScale(1, 10));
    EXPECT_EQ(8.0, amapCoordScale(1, 3));
    // type2 普通区域恒 2048×1024(任意 zoom)。
    EXPECT_EQ(4.0, amapCoordScale(2, 14));
    EXPECT_EQ(4.0, amapCoordScale(2, 10));
    // type2 大区域 kind 60/80 走 line-grid。
    EXPECT_EQ(2.0, amapCoordScale(2, 14, 60));
    EXPECT_EQ(4.0, amapCoordScale(2, 10, 80));
    EXPECT_NEAR(1.0 / 16.0, amapCoordScale(3, 14), 1e-9);
    EXPECT_EQ(2.0, amapCoordScale(4, 14));
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

// 参考 xinzhi-map amap_geometry.js normalizeEvenOddWinding:
// 高德区域环全部同向(even-odd 掩膜)。外环+被包围环(岛屿)同向时,直接
// nonzero 填充会把岛屿画成水面;归一化后外环 CCW(area>0)、孔 CW(area<0)
// 且孔紧跟外环。
TEST(AmapGeometryTest, EvenOddWindingGroupsOuterAndHole) {
    // 全部同向(CW,负面积)的环:大海外环 + 岛屿内环。
    std::vector<std::pair<double, double>> ocean = {
        {0, 0}, {100, 0}, {100, 100}, {0, 100}};
    std::vector<std::pair<double, double>> island = {
        {40, 40}, {60, 40}, {60, 60}, {40, 60}};
    // 同向:ocean 逆序 = island 同向序列(都 CW)。
    std::vector<std::pair<double, double>> oceanCW = ocean;
    std::reverse(oceanCW.begin(), oceanCW.end());
    const auto groups = amapNormalizeEvenOddWinding({oceanCW, island});

    ASSERT_EQ(2u, groups.size());
    // 外环重绕为 area>0(CCW),孔保持 area<0(CW)。
    auto area = [](const auto& r) {
        double sum = 0.0;
        const size_t n = r.size();
        for (size_t i = 0, j = n - 1; i < n; j = i++) {
            sum += (r[j].first + r[i].first) *
                   (r[j].second - r[i].second);
        }
        return sum / 2.0;
    };
    EXPECT_GT(area(groups[0]), 0.0);
    EXPECT_LT(area(groups[1]), 0.0);
}

// 两层嵌套:海(外)→ 岛(孔)→ 岛内湖(孔)。even-odd 深度:岛=1(孔)、
// 湖=2(外环)。归一化输出:海外环 + 岛孔,然后湖作为独立外环。
TEST(AmapGeometryTest, EvenOddWindingTwoLevelNesting) {
    std::vector<std::pair<double, double>> ocean = {
        {0, 0}, {200, 0}, {200, 200}, {0, 200}};
    std::vector<std::pair<double, double>> island = {
        {50, 50}, {150, 50}, {150, 150}, {50, 150}};
    std::vector<std::pair<double, double>> lake = {
        {80, 80}, {120, 80}, {120, 120}, {80, 120}};
    // 全部同向(CW)。
    std::vector<std::pair<double, double>> oceanCW = ocean;
    std::reverse(oceanCW.begin(), oceanCW.end());
    std::vector<std::pair<double, double>> islandCW = island;
    std::reverse(islandCW.begin(), islandCW.end());
    std::vector<std::pair<double, double>> lakeCW = lake;
    std::reverse(lakeCW.begin(), lakeCW.end());
    const auto groups = amapNormalizeEvenOddWinding(
        {oceanCW, islandCW, lakeCW});

    auto area = [](const auto& r) {
        double sum = 0.0;
        const size_t n = r.size();
        for (size_t i = 0, j = n - 1; i < n; j = i++) {
            sum += (r[j].first + r[i].first) *
                   (r[j].second - r[i].second);
        }
        return sum / 2.0;
    };
    ASSERT_EQ(3u, groups.size());
    // 海(外)+ 岛(孔)+ 湖(外)。
    EXPECT_GT(area(groups[0]), 0.0);
    EXPECT_LT(area(groups[1]), 0.0);
    EXPECT_GT(area(groups[2]), 0.0);
}

// 瓦片裁剪:越界环切进窗口,开口沿瓦片边闭合(参考 Sutherland–Hodgman)。
// canonical 空间 x∈[0,8192]、y∈[0,4096],窗口 ±256 buffer。
TEST(AmapGeometryTest, ClipPolygonRingKeepsInsideWindow) {
    // 环部分越界:x 到 9000(>8192+256),y 到 -100(< -256)。
    const std::vector<std::pair<double, double>> ring = {
        {-500, -500}, {9000, -500}, {9000, 4500}, {-500, 4500}};
    const auto clipped =
        amapClipPolygonRing(ring, -256.0, 8192.0 + 256.0,
                            -256.0, 4096.0 + 256.0);
    ASSERT_FALSE(clipped.empty());
    for (const auto& p : clipped) {
        EXPECT_GE(p.first, -256.0 - 1e-6);
        EXPECT_LE(p.first, 8192.0 + 256.0 + 1e-6);
        EXPECT_GE(p.second, -256.0 - 1e-6);
        EXPECT_LE(p.second, 4096.0 + 256.0 + 1e-6);
    }
    // 裁剪后环仍闭合到窗口边界(至少 4 角附近的点)。
    EXPECT_GE(clipped.size(), 4u);
}

// 解码 → 归一化 → Feature 分组:外环与孔进同一个 Polygon,孔不再独立成面。
TEST(AmapGeometryTest, DecodedPartGroupsHoleIntoPolygon) {
    AmapDecodedLayerPart part;
    part.z = 10;
    part.x = 815;
    part.y = 344;
    part.type = 2;
    AmapDecodedFeature f;
    f.classCode = 20000;
    f.geomType = 3;  // Polygon
    f.rings = {
        // 大海外环(同向 CW)。scale 4 → canonical x∈[2000,7200],
        // y∈[400,3600](翻转后 [496,3696])全在窗口内。
        {{500, 100}, {1800, 100}, {1800, 900}, {500, 900}},
        // 岛屿内环(同向 CW)。
        {{800, 200}, {1500, 200}, {1500, 700}, {800, 700}},
    };
    part.features.push_back(std::move(f));

    const auto features = amapDecodedPartToFeatures(part, /*toWgs84=*/false);
    // 一个 feature(外环 + 孔),不是两个独立面。
    ASSERT_EQ(1u, features.size());
    EXPECT_EQ(GeometryType::Polygon, features[0].type);
    ASSERT_EQ(2u, features[0].rings.size());
    // 孔被正确收集进 rings[1]。
    EXPECT_GT(features[0].rings[1].size(), 0u);
}

// 单环(无孔)区域:归一化只补闭合点(高德环开放),不改变绕向/坐标。
TEST(AmapGeometryTest, EvenOddWindingSingleRingUntouched) {
    std::vector<std::pair<double, double>> ring = {
        {100, 100}, {200, 100}, {200, 200}, {100, 200}};
    const auto groups = amapNormalizeEvenOddWinding({ring});
    ASSERT_EQ(1u, groups.size());
    // 开放环补闭合点 → size+1,首尾相同。
    ASSERT_EQ(ring.size() + 1, groups[0].size());
    for (size_t i = 0; i < ring.size(); ++i) {
        EXPECT_DOUBLE_EQ(ring[i].first, groups[0][i].first);
        EXPECT_DOUBLE_EQ(ring[i].second, groups[0][i].second);
    }
    EXPECT_DOUBLE_EQ(groups[0].front().first, groups[0].back().first);
    EXPECT_DOUBLE_EQ(groups[0].front().second, groups[0].back().second);
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
    // type2 区域的 kind 必须被解出(样式数据驱动配色的依据;旧实现恒 0)。
    bool sawRegionKind = false;
    for (const auto& p : parts) {
        if (p.type != 2) continue;
        const auto feats = amapDecodedPartToFeatures(p);
        for (const auto& feat : feats) {
            if (feat.properties.count("amap_kind")) {
                sawRegionKind = true;
                break;
            }
        }
        if (sawRegionKind) break;
    }
    EXPECT_TRUE(sawRegionKind) << "type2 region kind must be decoded";
}

// 真实样本:type2/3 多环 feature 应合并为「外环+孔」单 Polygon(不再每个环
// 独立成面)。样本含水面/绿地掩膜(海含岛、湖含岛),归一化前后 feature 数
// 应显著减少且部分 feature 携带孔环。
TEST(AmapGeometryTest, RealSampleHolesGroupedIntoPolygons) {
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
    size_t multiRingFeatures = 0;
    size_t polygonFeatures = 0;
    for (const auto& p : parts) {
        if (p.type != 2 && p.type != 3) continue;
        const auto feats = amapDecodedPartToFeatures(p);
        for (const auto& feat : feats) {
            if (feat.type != GeometryType::Polygon) continue;
            ++polygonFeatures;
            if (feat.rings.size() > 1) ++multiRingFeatures;
        }
    }
    // 重庆城区样本几乎必含带孔的水面/绿地(湖泊/岛屿);若样本恰好无孔,
    // 至少确认 feature 数不为零且不崩溃。
    EXPECT_GT(polygonFeatures, 0u);
    EXPECT_GT(multiRingFeatures, 0u)
        << "real sample should contain at least one polygon with holes";
}

// 归一化后的多环 polygon 必须能正常三角化(CDT 外环+孔 → 非空 fill)。
// 若孔环绕向/自交破坏 flood-fill,fill 为空 = 大面整体消失(真机近景
// 现象)。
TEST(AmapGeometryTest, RealSampleHolePolygonsTessellate) {
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
    const Ellipsoid& e = Ellipsoid::WGS84();
    size_t multiRing = 0;
    size_t emptyFill = 0;
    size_t tinyFill = 0;
    for (const auto& p : parts) {
        if (p.type != 2 && p.type != 3) continue;
        const auto feats = amapDecodedPartToFeatures(p);
        for (const auto& feat : feats) {
            if (feat.type != GeometryType::Polygon ||
                feat.rings.size() <= 1) {
                continue;
            }
            ++multiRing;
            // kind=0 的 type2 是行政区划/边界掩膜(200xx classCode),非
            // 水/绿地核心面,裁剪后退化可接受。
            const bool isCoreSurface =
                feat.properties.count("amap_kind") > 0;
            const auto fill = PolygonTessellator::tessellate(feat, e);
            if (fill.fillIndices.empty()) {
                if (isCoreSurface) ++emptyFill;
            } else if (fill.fillIndices.size() < 6) {
                ++tinyFill;
            }
        }
    }
    EXPECT_GT(multiRing, 0u);
    // 裁剪可能让少量跨边界掩膜面退化(孔盖外环);容忍少数,绝大多数须正常。
    EXPECT_EQ(emptyFill, 0u)
        << "core surface (kind) hole polygons must tessellate";
    EXPECT_EQ(tinyFill, 0u) << "hole polygons must not degenerate";
}
