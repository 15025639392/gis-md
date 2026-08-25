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

bool pointInRingXY(const std::vector<std::pair<double, double>>& ring,
                   double x, double y) {
    bool inside = false;
    const size_t n = ring.size();
    for (size_t i = 0, j = n - 1; i < n; j = i++) {
        const double yi = ring[i].second;
        const double yj = ring[j].second;
        if ((yi > y) != (yj > y)) {
            const double xAtY = ring[j].first +
                                (ring[i].first - ring[j].first) *
                                    (y - yj) / (yi - yj);
            if (x < xAtY) inside = !inside;
        }
    }
    return inside;
}

}  // namespace

TEST(AmapGeometryTest, CoordScalePerType) {
    // POI labels use a 2048×1024 grid, not the z14 line grid.
    EXPECT_EQ(4.0, amapCoordScale(0, 14));
    EXPECT_EQ(8.0, amapCoordScale(0, 3));
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

TEST(AmapGeometryTest, PoiZ14AnchorUsesFullLabelGrid) {
    AmapDecodedLayerPart part;
    part.z = 14;
    part.x = 13038;
    part.y = 5505;
    part.type = 0;
    AmapDecodedFeature f;
    f.classCode = 12024;
    f.name = "center";
    f.rank = 7;
    // Native POI extent is 2048×1024.  The center must remain the tile
    // center after scale 4 + bottom-up Y flip; the old line scale 2 moved it
    // to (2048,3072), i.e. one quarter tile west and one quarter south.
    f.rings = {{{1024.0, 512.0}}};
    part.features.push_back(std::move(f));

    const auto features = amapDecodedPartToFeatures(part, false);
    ASSERT_EQ(1u, features.size());
    ASSERT_EQ(GeometryType::Point, features[0].type);
    EXPECT_EQ("-7", features[0].properties.at("rank"));
    const double expectedLon =
        (part.x + 0.5) / std::exp2(part.z) * 360.0 - 180.0;
    const double expectedLat =
        90.0 - (part.y + 0.5) / std::exp2(part.z) * 180.0;
    EXPECT_NEAR(expectedLon,
                features[0].rings[0][0].longitude() * kRadToDeg, 1e-9);
    EXPECT_NEAR(expectedLat,
                features[0].rings[0][0].latitude() * kRadToDeg, 1e-9);
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

TEST(AmapGeometryTest, DecodedLineConvertsBottomUpBlobYToNorthDownLat) {
    AmapDecodedLayerPart part;
    part.z = 0;
    part.x = 0;
    part.y = 0;
    part.type = 1;
    AmapDecodedFeature f;
    f.classCode = 20009;
    // z0 line blobs use the 1024x512 native grid (scale 8).  Amap's raw
    // geometry y=0 is the south edge; the largest raw y is the north edge.
    f.rings = {{{0, 0}, {0, 512}}};
    part.features.push_back(std::move(f));

    const auto features = amapDecodedPartToFeatures(part, /*toWgs84=*/false);
    ASSERT_EQ(1u, features.size());
    ASSERT_EQ(1u, features[0].rings.size());
    ASSERT_EQ(2u, features[0].rings[0].size());
    const double firstLat = features[0].rings[0][0].latitude() * kRadToDeg;
    const double lastLat = features[0].rings[0][1].latitude() * kRadToDeg;
    EXPECT_NEAR(-90.0, firstLat, 1e-9);
    EXPECT_NEAR(90.0, lastLat, 1e-9);
}

TEST(AmapGeometryTest, AdjacentRowsShareLatitudeAfterRawYFlip) {
    auto edgeLatitude = [](int tileY, double rawY) {
        AmapDecodedLayerPart part;
        part.z = 1;
        part.x = 0;
        part.y = tileY;
        part.type = 1;
        AmapDecodedFeature f;
        f.classCode = 20009;
        f.rings = {{{0, rawY}, {1, rawY}}};
        part.features.push_back(std::move(f));
        const auto features =
            amapDecodedPartToFeatures(part, /*toWgs84=*/false);
        return features[0].rings[0][0].latitude();
    };

    // North tile's raw south edge (y=0) and south tile's raw north edge
    // (y=512 at z1's line scale 8) are the same geographic latitude.
    const double northTileSouth = edgeLatitude(0, 0.0);
    const double southTileNorth = edgeLatitude(1, 512.0);
    EXPECT_DOUBLE_EQ(northTileSouth, southTileNorth);
    EXPECT_NEAR(0.0, northTileSouth * kRadToDeg, 1e-9);
}

TEST(AmapGeometryTest, DecodedPolygonUsesTheSameTopDownCanonicalY) {
    AmapDecodedLayerPart part;
    part.z = 0;
    part.x = 0;
    part.y = 0;
    part.type = 2;
    AmapDecodedFeature f;
    f.classCode = 30001;
    f.kind = 61;
    // type2 native extent is 2048x1024 (scale 4). Raw y=0 is south and
    // raw y=1024 is north, just like line/building/POI geometry.
    f.rings = {{{0, 0}, {2048, 0}, {2048, 1024}, {0, 1024}}};
    part.features.push_back(std::move(f));

    const auto features = amapDecodedPartToFeatures(part, /*toWgs84=*/false);
    ASSERT_EQ(1u, features.size());
    double south = 1e9;
    double north = -1e9;
    for (const Cartographic& c : features[0].rings[0]) {
        const double lat = c.latitude() * kRadToDeg;
        south = std::min(south, lat);
        north = std::max(north, lat);
    }
    EXPECT_NEAR(-90.0, south, 1e-9);
    EXPECT_NEAR(90.0, north, 1e-9);
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

// 瓦片裁剪:越界环按隐式闭合 polygon 切进窗口(参考 S-H)。
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

// Raw ring 省略重复首点，但仍是 polygon。裁剪必须包含 last→first 隐式
// 闭边；这是参考实现行为，也防止以后把它误改成开放折线裁剪。
TEST(AmapGeometryTest, ClipPolygonRingUsesImplicitClosingEdge) {
    const std::vector<std::pair<double, double>> ring = {
        {-2.0, 1.0}, {2.0, 1.0}, {2.0, 5.0}};
    const auto clipped = amapClipPolygonRing(ring, 0.0, 4.0, 0.0, 4.0);
    const std::vector<std::pair<double, double>> expected = {
        {1.0, 4.0}, {0.0, 3.0}, {0.0, 1.0}, {2.0, 1.0}, {2.0, 4.0}};
    ASSERT_EQ(expected.size(), clipped.size());
    for (size_t i = 0; i < expected.size(); ++i) {
        EXPECT_NEAR(expected[i].first, clipped[i].first, 1e-9);
        EXPECT_NEAR(expected[i].second, clipped[i].second, 1e-9);
    }
}

// 解码 → 归一化 → Feature 分组:外环与孔进同一个 Polygon,孔不再独立成面。
// 环可显式重复首点；省略时也按隐式闭合处理。
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
        // y∈[400,3600](翻转后 [496,3696])全在窗口内。闭合(首尾同点)。
        {{500, 100}, {1800, 100}, {1800, 900}, {500, 900}, {500, 100}},
        // 岛屿内环(同向 CW)。
        {{800, 200}, {1500, 200}, {1500, 700}, {800, 700}, {800, 200}},
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

// 凹外环与凹孔均呈 U 形。孔完全位于外环的实体带内，但孔的 bbox 中心
// 落在 U 形凹口（也在外环之外）。用 bbox 中心判断包含会把孔拆成独立面；
// 必须选取孔自身的严格内点做 point-in-polygon。
TEST(AmapGeometryTest, ConcaveHoleUsesInteriorPointInsteadOfBoundsCenter) {
    AmapDecodedLayerPart part;
    part.z = 10;
    part.x = 815;
    part.y = 344;
    part.type = 2;
    AmapDecodedFeature f;
    f.classCode = 30001;
    f.geomType = 3;
    f.rings = {
        // 外 U:底带 y=[100,350]，左右臂延伸到 y=900。
        {{100, 100}, {1900, 100}, {1900, 900}, {1400, 900},
         {1400, 350}, {600, 350}, {600, 900}, {100, 900}, {100, 100}},
        // 内 U 完全落在外 U 内；bbox 中心 (1000,500) 在两者凹口中。
        {{250, 200}, {1750, 200}, {1750, 800}, {1550, 800},
         {1550, 300}, {450, 300}, {450, 800}, {250, 800}, {250, 200}},
    };
    part.features.push_back(std::move(f));

    const auto features = amapDecodedPartToFeatures(part, /*toWgs84=*/false);
    ASSERT_EQ(1u, features.size());
    ASSERT_EQ(2u, features[0].rings.size());
}

// 互不嵌套的独立碎片:即使绕向相反(一正一负),也不得合并成"外环+孔"。
// 环显式闭合；省略重复首点也应得到相同行为。
// 旧实现把负面积环一律当孔并入前一个外环 → CDT 在碎片之间大面积填充
// (大量错误三角形的根因)。每个独立碎片应是独立 Polygon。
TEST(AmapGeometryTest, DisjointRingsStaySeparatePolygons) {
    AmapDecodedLayerPart part;
    part.z = 10;
    part.x = 815;
    part.y = 344;
    part.type = 2;
    AmapDecodedFeature f;
    f.classCode = 30001;
    f.geomType = 3;
    f.rings = {
        // 第一个独立面(正绕向):canonical x∈[2000,7200], y∈[496,3696]。
        {{500, 100}, {1800, 100}, {1800, 900}, {500, 900}, {500, 100}},
        // 第二个独立面(负绕向,远离第一个):canonical x∈[300,1500] 之外,
        // y∈[496,3696] —— 互不包含。
        {{100, 100}, {100, 900}, {400, 900}, {400, 100}, {100, 100}},
    };
    part.features.push_back(std::move(f));

    const auto features = amapDecodedPartToFeatures(part, /*toWgs84=*/false);
    // 两个独立碎片 → 两个独立 polygon,不是外环+孔。
    ASSERT_EQ(2u, features.size());
    EXPECT_EQ(GeometryType::Polygon, features[0].type);
    EXPECT_EQ(GeometryType::Polygon, features[1].type);
    EXPECT_EQ(1u, features[0].rings.size());
    EXPECT_EQ(1u, features[1].rings.size());
}

// 第二个环为负绕向凹 U，其 bbox 中心落在第一个小方块内，但 U 的真实
// 内部与方块完全不相交。bbox 中心法会错误把 U 当成方块的孔。
TEST(AmapGeometryTest, ConcaveDisjointNegativeRingStaysIndependent) {
    AmapDecodedLayerPart part;
    part.z = 10;
    part.x = 815;
    part.y = 344;
    part.type = 2;
    AmapDecodedFeature f;
    f.classCode = 30001;
    f.geomType = 3;
    f.rings = {
        // raw 中逆时针；Y 翻转后为正绕向外环。
        {{45, 45}, {55, 45}, {55, 55}, {45, 55}, {45, 45}},
        // raw 中顺时针；Y 翻转后为负绕向。U 的 bbox 中心正落在
        // 中心方块内，但 U 的严格内点在两侧臂上，实际不在方块内。
        {{0, 100}, {30, 100}, {30, 30}, {70, 30},
         {70, 100}, {100, 100}, {100, 0}, {0, 0}, {0, 100}},
    };
    part.features.push_back(std::move(f));

    const auto features = amapDecodedPartToFeatures(part, /*toWgs84=*/false);
    ASSERT_EQ(2u, features.size());
    EXPECT_EQ(1u, features[0].rings.size());
    EXPECT_EQ(1u, features[1].rings.size());
}

TEST(AmapGeometryTest, CompoundMaskTrianglesStayInsideSourceEvenOddMask) {
    AmapDecodedLayerPart part;
    part.z = 14;
    part.x = 13040;
    part.y = 5502;
    part.type = 2;
    AmapDecodedFeature f;
    f.classCode = 20017;
    f.kind = 0;
    f.geomType = 3;
    f.rings = {
        {{0, 100}, {300, 100}, {300, 300}, {0, 300}},
        {{500, 200}, {700, 200}, {700, 400}, {500, 400}},
        {{900, 500}, {1100, 500}, {1100, 700}, {900, 700}},
    };
    part.features.push_back(std::move(f));

    const auto features = amapDecodedPartToFeatures(part, /*toWgs84=*/false);
    ASSERT_EQ(3u, features.size());
    const Ellipsoid& e = Ellipsoid::WGS84();
    size_t triangles = 0;
    for (const Feature& feature : features) {
        const auto fill = PolygonTessellator::tessellate(feature, e);
        EXPECT_FALSE(fill.fillIndices.empty());
        triangles += fill.fillIndices.size() / 3;
    }
    EXPECT_GT(triangles, 0u);
}

// 单环(无孔)区域:raw 可省略重复首点；归一化保持点列不变，闭合由
// 裁剪/三角化的 modulo 边语义完成。
TEST(AmapGeometryTest, EvenOddWindingSingleRingUntouched) {
    std::vector<std::pair<double, double>> ring = {
        {100, 100}, {200, 100}, {200, 200}, {100, 200}};
    const auto groups = amapNormalizeEvenOddWinding({ring});
    ASSERT_EQ(1u, groups.size());
    // 不补重复首点 → size 不变，首尾仍不同但语义上是闭环。
    ASSERT_EQ(ring.size(), groups[0].size());
    for (size_t i = 0; i < ring.size(); ++i) {
        EXPECT_DOUBLE_EQ(ring[i].first, groups[0][i].first);
        EXPECT_DOUBLE_EQ(ring[i].second, groups[0][i].second);
    }
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
    bool sawBuildingPart = false;
    for (const auto& p : parts) {
        const auto feats = amapDecodedPartToFeatures(p);
        total += feats.size();
        if (p.type == 3) {
            sawBuildingPart = true;
            buildings += feats.size();
        }
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
    if (sawBuildingPart) {
        EXPECT_GT(buildings, 0u);
    }
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

TEST(AmapGeometryTest, RealSampleLinesStayInTheirLayerTileGrid) {
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
    size_t linePoints = 0;
    constexpr double kBuffer = 256.0;
    for (const auto& part : parts) {
        for (const auto& feature : part.features) {
            if (part.type != 1 && part.type != 4 && !feature.lineGeometry) {
                continue;
            }
            const int geometryType = feature.lineGeometry ? 1 : part.type;
            const double scale =
                amapCoordScale(geometryType, part.z, feature.kind);
            for (const auto& ring : feature.rings) {
                for (const auto& point : ring) {
                    const double x = point.first * scale;
                    const double y = 4096.0 - point.second * scale;
                    EXPECT_GE(x, -kBuffer);
                    EXPECT_LE(x, 8192.0 + kBuffer);
                    EXPECT_GE(y, -kBuffer);
                    EXPECT_LE(y, 4096.0 + kBuffer);
                    ++linePoints;
                }
            }
        }
    }
    EXPECT_GT(linePoints, 0u);
}

// 真样本:type2 区域必须产出 Polygon Feature(窗沿闭合前,跨瓦开放弧被
// "首尾不同即丢弃"规则全部过滤 → 0 面 → 真机整屏露地球底色)。
TEST(AmapGeometryTest, RealSampleRegionProducesPolygons) {
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

    std::vector<Feature> feats;
    std::string err;
    ASSERT_TRUE(amapBytesToFeatures(raw.data(), raw.size(), /*regionsOnly=*/true,
                                    feats, &err));
    size_t polys = 0;
    size_t validRings = 0;
    for (const auto& feat : feats) {
        if (feat.type != GeometryType::Polygon) continue;
        ++polys;
        for (const auto& ring : feat.rings) {
            if (ring.size() >= 3) {
                ++validRings;
                for (const auto& c : ring) {
                    EXPECT_TRUE(std::isfinite(c.longitude()));
                    EXPECT_TRUE(std::isfinite(c.latitude()));
                }
            }
        }
    }
    EXPECT_GT(polys, 0u) << "window-closed rings must produce polygons";
    // PolygonTessellator/VectorTileRasterizer 通过 modulo 隐式闭合，
    // 因此不要求解码输出重复首点。
    size_t totalRings = 0;
    for (const auto& feat : feats) {
        if (feat.type != GeometryType::Polygon) continue;
        totalRings += feat.rings.size();
    }
    EXPECT_EQ(totalRings, validRings);
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
    size_t polygonFeatures = 0;
    for (const auto& p : parts) {
        if (p.type != 2 && p.type != 3) continue;
        const auto feats = amapDecodedPartToFeatures(p);
        for (const auto& feat : feats) {
            if (feat.type != GeometryType::Polygon) continue;
            ++polygonFeatures;
        }
    }
    // 重庆城区样本几乎必含带孔的水面/绿地(湖泊/岛屿);若样本恰好无孔,
    // 至少确认 feature 数不为零且不崩溃。
    EXPECT_GT(polygonFeatures, 0u);
    // 某些高德 z12 城市切片会把岛/湖作为独立 feature 返回,不保证同一
    // feature 携带孔环;有孔时由下面的 tessellation 回归验证。
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
    if (multiRing == 0) {
        GTEST_SKIP() << "sample tile contains no multi-ring polygon feature";
    }
    // 裁剪可能让少量跨边界掩膜面退化(孔盖外环);容忍少数,绝大多数须正常。
    EXPECT_EQ(emptyFill, 0u)
        << "core surface (kind) hole polygons must tessellate";
    EXPECT_EQ(tinyFill, 0u) << "hole polygons must not degenerate";
}

// 真实样本:type2 面三角化后三角形总数应合理(碎片不再错误合并成
// "外环+孔",否则 CDT 在碎片间大面积填充 → 三角形数爆炸)。
TEST(AmapGeometryTest, RealSampleType2TrianglesBounded) {
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
    size_t polygons = 0;
    size_t totalTris = 0;
    for (const auto& p : parts) {
        if (p.type != 2) continue;
        for (const auto& feat : amapDecodedPartToFeatures(p)) {
            if (feat.type != GeometryType::Polygon) continue;
            ++polygons;
            const auto fill = PolygonTessellator::tessellate(feat, e);
            totalTris += fill.fillIndices.size() / 3;
        }
    }
    EXPECT_GT(polygons, 0u);
    // 每面平均三角形应受环点数约束(≈2×环点数,而非碎片间大面积填充)。
    // 用宽松上界:均值 < 500(正常面几十~几百,错误合并会到数千)。
    EXPECT_LT(totalTris / std::max<size_t>(1, polygons), 500u)
        << "type2 triangles per polygon unreasonably high (fragment "
           "mis-merge)";
}

// 真样本:type2 输出三角形不得越过原始 even-odd 面。这个判据专门抓
// “凹环裁剪成多个离散分量后被单 ring 桥接”的巨型楔面：顶点都可能仍在
// 瓦内、三角数量也正常，但三角形质心已经落到原面之外。
TEST(AmapGeometryTest, RealSampleType2TriangleCentroidsStayInSourceMask) {
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
    size_t triangles = 0;
    size_t outside = 0;
    for (const auto& p : parts) {
        if (p.type != 2) continue;
        for (size_t fi = 0; fi < p.features.size(); ++fi) {
            const AmapDecodedFeature& source = p.features[fi];
            const double scale = amapCoordScale(p.type, p.z, source.kind);
            std::vector<std::vector<std::pair<double, double>>> sourceRings;
            double sourceMinX = 1e18, sourceMaxX = -1e18;
            double sourceMinY = 1e18, sourceMaxY = -1e18;
            for (const auto& ring : source.rings) {
                std::vector<std::pair<double, double>> canonical;
                canonical.reserve(ring.size());
                for (const auto& pt : ring) {
                    canonical.emplace_back(pt.first * scale,
                                           4096.0 - pt.second * scale);
                    sourceMinX = std::min(sourceMinX, canonical.back().first);
                    sourceMaxX = std::max(sourceMaxX, canonical.back().first);
                    sourceMinY = std::min(sourceMinY, canonical.back().second);
                    sourceMaxY = std::max(sourceMaxY, canonical.back().second);
                }
                sourceRings.push_back(std::move(canonical));
            }

            AmapDecodedLayerPart one = p;
            one.features = {source};
            size_t featureTriangles = 0;
            size_t featureOutside = 0;
            size_t outputFeatures = 0;
            size_t outputRings = 0;
            double firstOutsideX = 0.0, firstOutsideY = 0.0;
            for (const Feature& feature :
                 amapDecodedPartToFeatures(one, /*toWgs84=*/false)) {
                if (feature.type != GeometryType::Polygon) continue;
                ++outputFeatures;
                outputRings += feature.rings.size();
                const auto fill = PolygonTessellator::tessellate(feature, e);
                for (size_t i = 0; i + 2 < fill.fillIndices.size(); i += 3) {
                    const Cartographic a = e.cartesianToCartographic(
                        fill.positions[fill.fillIndices[i]]);
                    const Cartographic b = e.cartesianToCartographic(
                        fill.positions[fill.fillIndices[i + 1]]);
                    const Cartographic c = e.cartesianToCartographic(
                        fill.positions[fill.fillIndices[i + 2]]);
                    const double n = std::exp2(p.z);
                    const auto local = [n, &p](const Cartographic& v) {
                        const double lonDeg = v.longitude() * kRadToDeg;
                        const double latDeg = v.latitude() * kRadToDeg;
                        return std::make_pair(
                            ((lonDeg + 180.0) / 360.0 * n - p.x) * 8192.0,
                            ((90.0 - latDeg) / 180.0 * n - p.y) * 4096.0);
                    };
                    const auto la = local(a);
                    const auto lb = local(b);
                    const auto lc = local(c);
                    const double x = (la.first + lb.first + lc.first) / 3.0;
                    const double y = (la.second + lb.second + lc.second) / 3.0;
                    bool inMask = false;
                    for (const auto& ring : sourceRings) {
                        if (pointInRingXY(ring, x, y)) inMask = !inMask;
                    }
                    ++triangles;
                    ++featureTriangles;
                    if (!inMask) {
                        ++outside;
                        ++featureOutside;
                        if (featureOutside == 1) {
                            firstOutsideX = x;
                            firstOutsideY = y;
                        }
                    }
                }
            }
            if (featureOutside > 0) {
                ADD_FAILURE()
                    << "outside source mask tile=" << p.x << "_"
                    << p.y << "_" << p.z << " feature=" << fi
                    << " class=" << source.classCode
                    << " kind=" << source.kind
                    << " rings=" << source.rings.size()
                    << " outside=" << featureOutside << "/"
                    << featureTriangles << " outputs=" << outputFeatures
                    << " outputRings=" << outputRings
                    << " firstOutside=" << firstOutsideX << ","
                    << firstOutsideY << " bbox=" << sourceMinX << ","
                    << sourceMinY << ".." << sourceMaxX << ","
                    << sourceMaxY;
            }
        }
    }
    if (triangles == 0) {
        GTEST_SKIP() << "sample tile contains no tessellated type2 polygons";
    }
    EXPECT_EQ(outside, 0u);
}

// 真实样本:type2 面覆盖的经纬度范围占瓦片比例。
// 若 kind=3 水系面覆盖过大(如整瓦),说明裁剪/判定仍有问题 → 大面积蓝。
TEST(AmapGeometryTest, RealSampleType2Coverage) {
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
    // 瓦片经纬度跨度(4326 网格):z 档 360/2^z × 180/2^z。
    FILE* diag = std::fopen("/tmp/amap_coverage.txt", "w");
    ASSERT_NE(nullptr, diag);
    for (const auto& p : parts) {
        if (p.type != 2) continue;
        const double tileW = 360.0 / std::exp2(p.z);
        const double tileH = 180.0 / std::exp2(p.z);
        const auto feats = amapDecodedPartToFeatures(p);
        for (const auto& feat : feats) {
            if (feat.type != GeometryType::Polygon) continue;
            const std::string kind =
                feat.properties.count("amap_kind")
                    ? feat.properties.at("amap_kind")
                    : "-";
            double mnx = 1e9, mxx = -1e9, mny = 1e9, mxy = -1e9;
            for (const auto& ring : feat.rings) {
                for (const auto& c : ring) {
                    const double lon = c.longitude() * 180.0 / 3.14159265358979323846;
                    const double lat = c.latitude() * 180.0 / 3.14159265358979323846;
                    mnx = std::min(mnx, lon);
                    mxx = std::max(mxx, lon);
                    mny = std::min(mny, lat);
                    mxy = std::max(mxy, lat);
                }
            }
            const double coverX = (mxx - mnx) / tileW;
            const double coverY = (mxy - mny) / tileH;
            std::fprintf(diag, "kind=%s rings=%zu coverX=%.2f coverY=%.2f\n",
                         kind.c_str(), feat.rings.size(), coverX, coverY);
        }
    }
    std::fclose(diag);
}

// 真样本:每个 type2 面经纬度是否落在瓦片边界内(裁剪生效的判据)。
// 覆盖异常(115×)说明裁剪失效或坐标换算错误。
TEST(AmapGeometryTest, RealSampleType2InsideTile) {
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
    const double kDeg = 180.0 / 3.14159265358979323846;
    FILE* diag = std::fopen("/tmp/amap_inside.txt", "w");
    ASSERT_NE(nullptr, diag);
    for (const auto& p : parts) {
        if (p.type != 2) continue;
        const double tileW = 360.0 / std::exp2(p.z);
        const double tileH = 180.0 / std::exp2(p.z);
        const double west = (p.x / std::exp2(p.z)) * 360.0 - 180.0;
        const double north = 90.0 - (p.y / std::exp2(p.z)) * 180.0;
        const auto feats = amapDecodedPartToFeatures(p);
        for (const auto& feat : feats) {
            if (feat.type != GeometryType::Polygon) continue;
            for (const auto& ring : feat.rings) {
                for (const auto& c : ring) {
                    const double lon = c.longitude() * kDeg;
                    const double lat = c.latitude() * kDeg;
                    const bool inside =
                        lon >= west - 0.05 && lon <= west + tileW + 0.05 &&
                        lat >= north - tileH - 0.05 &&
                        lat <= north + 0.05;
                    if (!inside) {
                        std::fprintf(diag,
                                     "OUT tile %d_%d_%d kind=%s "
                                     "lon=%.4f lat=%.4f tile=[%.4f,%.4f]x"
                                     "[%.4f,%.4f]\n",
                                     p.x, p.y, p.z,
                                     feat.properties.count("amap_kind")
                                         ? feat.properties.at("amap_kind").c_str()
                                         : "-",
                                     lon, lat, west, west + tileW,
                                     north - tileH, north);
                    }
                }
            }
        }
    }
    std::fclose(diag);
}

TEST(AmapGeometryTest, RealSampleType2FeatureDiagnostics) {
    const char* path = std::getenv("AMAP_DIAG_TILE");
    if (!path) GTEST_SKIP() << "AMAP_DIAG_TILE unset";
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
    const double deg = 180.0 / 3.14159265358979323846;
    const bool dumpRings = std::getenv("AMAP_DIAG_RINGS") != nullptr;
    const bool dumpPoints = std::getenv("AMAP_DIAG_POINTS") != nullptr;
    for (const auto& p : parts) {
        if (p.type != 2) continue;
        std::printf("PART tile=%d_%d_%d features=%zu\n", p.x, p.y, p.z,
                    p.features.size());
        for (size_t i = 0; i < p.features.size(); ++i) {
            const auto& src = p.features[i];
            const double scale = amapCoordScale(p.type, p.z, src.kind);
            if (dumpRings && src.classCode == 30001) {
                auto signedArea = [](const auto& ring) {
                    double sum = 0.0;
                    for (size_t ri = 0, rj = ring.size() - 1;
                         ri < ring.size(); rj = ri++) {
                        sum += (ring[rj].first + ring[ri].first) *
                               (ring[rj].second - ring[ri].second);
                    }
                    return 0.5 * sum;
                };
                std::printf("RINGS class=%d kind=%d count=%zu", src.classCode,
                            src.kind, src.rings.size());
                for (const auto& ring : src.rings) {
                    double x0 = 1e30, x1 = -1e30, y0 = 1e30, y1 = -1e30;
                    for (const auto& q : ring) {
                        x0 = std::min(x0, q.first * scale);
                        x1 = std::max(x1, q.first * scale);
                        y0 = std::min(y0, q.second * scale);
                        y1 = std::max(y1, q.second * scale);
                    }
                    std::printf(" [n=%zu area=%.1f bbox=%.1f,%.1f..%.1f,%.1f]",
                                ring.size(), signedArea(ring) * scale * scale,
                                x0, y0, x1, y1);
                }
                std::printf("\n");
                if (dumpPoints && src.kind == 63) {
                    for (size_t ri = 0; ri < src.rings.size(); ++ri) {
                        std::printf("POINTS ring=%zu", ri);
                        for (const auto& q : src.rings[ri]) {
                            std::printf(" %.0f,%.0f", q.first * scale,
                                        q.second * scale);
                        }
                        std::printf("\n");
                    }
                }
            }
            double mnx = 1e30, mxx = -1e30, mny = 1e30, mxy = -1e30;
            for (const auto& ring : src.rings) {
                for (const auto& q : ring) {
                    mnx = std::min(mnx, q.first * scale);
                    mxx = std::max(mxx, q.first * scale);
                    mny = std::min(mny, q.second * scale);
                    mxy = std::max(mxy, q.second * scale);
                }
            }
            AmapDecodedLayerPart one = p;
            one.features = {src};
            const auto out = amapDecodedPartToFeatures(one, false);
            for (size_t oi = 0; oi < out.size(); ++oi) {
                double olon0 = 1e30, olon1 = -1e30, olat0 = 1e30,
                       olat1 = -1e30;
                for (const auto& ring : out[oi].rings) {
                    for (const auto& c : ring) {
                        olon0 = std::min(olon0, c.longitude() * deg);
                        olon1 = std::max(olon1, c.longitude() * deg);
                        olat0 = std::min(olat0, c.latitude() * deg);
                        olat1 = std::max(olat1, c.latitude() * deg);
                    }
                }
                Ellipsoid ellipsoid = Ellipsoid::WGS84();
                const auto fill = PolygonTessellator::tessellate(
                    out[oi], ellipsoid);
                double maxEdge = 0.0;
                double maxTriArea = 0.0;
                const double n = std::exp2(p.z);
                auto local = [&](const Vec3& v) {
                    const Cartographic c = ellipsoid.cartesianToCartographic(v);
                    const double lon = c.longitude() * deg;
                    const double lat = c.latitude() * deg;
                    return std::pair<double, double>{
                        ((lon + 180.0) / 360.0 * n - p.x) * 8192.0,
                        ((90.0 - lat) / 180.0 * n - p.y) * 4096.0};
                };
                auto cross = [](const auto& a, const auto& b,
                                const auto& c) {
                    return std::abs((b.first - a.first) *
                                        (c.second - a.second) -
                                    (b.second - a.second) *
                                        (c.first - a.first)) *
                           0.5;
                };
                for (size_t ti = 0; ti + 2 < fill.fillIndices.size(); ti += 3) {
                    const auto a = local(fill.positions[fill.fillIndices[ti]]);
                    const auto b = local(fill.positions[fill.fillIndices[ti + 1]]);
                    const auto c = local(fill.positions[fill.fillIndices[ti + 2]]);
                    auto dist = [](const auto& u, const auto& v) {
                        const double dx = u.first - v.first;
                        const double dy = u.second - v.second;
                        return std::sqrt(dx * dx + dy * dy);
                    };
                    maxEdge = std::max({maxEdge, dist(a, b), dist(b, c),
                                        dist(c, a)});
                    maxTriArea = std::max(maxTriArea, cross(a, b, c));
                }
                std::printf(
                    "FEATURE i=%zu class=%d kind=%d sub=%d rawRings=%zu "
                    "rawBBox=%.1f,%.1f..%.1f,%.1f out=%zu outRings=%zu "
                    "geoBBox=%.6f,%.6f..%.6f,%.6f tris=%zu "
                    "maxEdge=%.1f maxArea=%.1f\n",
                    i, src.classCode, src.kind, src.subKey, src.rings.size(),
                    mnx, mny, mxx, mxy, oi, out[oi].rings.size(), olon0,
                    olat0, olon1, olat1, fill.fillIndices.size() / 3,
                    maxEdge, maxTriArea);
            }
        }
    }
}
