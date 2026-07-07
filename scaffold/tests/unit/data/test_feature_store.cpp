#include <gtest/gtest.h>

#include "earth_engine/data/FeatureStore.h"
#include "earth_engine/data/GeoJsonImporter.h"
#include "earth_engine/core/geodesy/Cartographic.h"
#include "earth_engine/core/geodesy/Transforms.h"
#include "earth_engine/core/math/Rectangle.h"

#include <set>
#include <string>

using namespace earth_engine;

namespace {

Feature makePolygon(FeatureId presetId, double west, double south,
                    double east, double north) {
    Feature f;
    f.id = presetId;
    f.type = GeometryType::Polygon;
    f.rings = {{Cartographic(west, south), Cartographic(east, south),
                Cartographic(east, north), Cartographic(west, north),
                Cartographic(west, south)}};
    // bounds 故意留空,验证 store 从 rings 兜底计算。
    return f;
}

std::set<FeatureId> toSet(const std::vector<FeatureId>& v) {
    return std::set<FeatureId>(v.begin(), v.end());
}

} // namespace

// ============================================================
// 增删查
// ============================================================

TEST(FeatureStoreTest, AddAssignsStableIdAndGet) {
    FeatureStore store;
    Feature f = makePolygon(kInvalidFeatureId, 0.0, 0.0, 0.1, 0.1);
    FeatureId id = store.addFeature(std::move(f));
    EXPECT_NE(kInvalidFeatureId, id);
    EXPECT_EQ(1u, store.size());

    const Feature* got = store.getFeature(id);
    ASSERT_NE(nullptr, got);
    EXPECT_EQ(id, got->id);
    EXPECT_EQ(GeometryType::Polygon, got->type);
}

TEST(FeatureStoreTest, BoundsComputedFromRingsWhenEmpty) {
    FeatureStore store;
    FeatureId id = store.addFeature(makePolygon(kInvalidFeatureId, 0.2, 0.3, 0.5, 0.7));
    const Feature* got = store.getFeature(id);
    ASSERT_NE(nullptr, got);
    EXPECT_NEAR(0.2, got->bounds.west(), 1e-12);
    EXPECT_NEAR(0.3, got->bounds.south(), 1e-12);
    EXPECT_NEAR(0.5, got->bounds.east(), 1e-12);
    EXPECT_NEAR(0.7, got->bounds.north(), 1e-12);
}

TEST(FeatureStoreTest, PresetIdAdvancesAllocator) {
    FeatureStore store;
    store.addFeature(makePolygon(100, 0, 0, 0.1, 0.1));
    // 下一条自动分配的 ID 应 > 100,避免冲突。
    FeatureId autoId = store.addFeature(makePolygon(kInvalidFeatureId, 1, 1, 1.1, 1.1));
    EXPECT_GT(autoId, 100u);
    EXPECT_EQ(2u, store.size());
}

TEST(FeatureStoreTest, RemoveFeature) {
    FeatureStore store;
    FeatureId id = store.addFeature(makePolygon(kInvalidFeatureId, 0, 0, 0.1, 0.1));
    EXPECT_TRUE(store.removeFeature(id));
    EXPECT_EQ(0u, store.size());
    EXPECT_EQ(nullptr, store.getFeature(id));
    EXPECT_TRUE(store.queryVisible(Rectangle(-1, -1, 1, 1)).empty());
    EXPECT_FALSE(store.removeFeature(id));  // 已删,再删失败
}

// ============================================================
// 视口查询
// ============================================================

TEST(FeatureStoreTest, QueryVisibleReturnsIntersecting) {
    FeatureStore store;
    FeatureId a = store.addFeature(makePolygon(kInvalidFeatureId, 0.0, 0.0, 0.1, 0.1));
    FeatureId b = store.addFeature(makePolygon(kInvalidFeatureId, 5.0, 5.0, 5.1, 5.1));
    FeatureId c = store.addFeature(makePolygon(kInvalidFeatureId, 0.05, 0.05, 0.2, 0.2));

    auto hit = toSet(store.queryVisible(Rectangle(-0.5, -0.5, 0.15, 0.15)));
    EXPECT_EQ(2u, hit.size());
    EXPECT_TRUE(hit.count(a));
    EXPECT_TRUE(hit.count(c));
    EXPECT_FALSE(hit.count(b));
}

// ============================================================
// 编辑更新:重建索引 + version 递增
// ============================================================

TEST(FeatureStoreTest, UpdateFeatureReindexesAndBumpsVersion) {
    FeatureStore store;
    FeatureId id = store.addFeature(makePolygon(kInvalidFeatureId, 0.0, 0.0, 0.1, 0.1));
    ASSERT_EQ(1u, store.getFeature(id)->version);

    // 把几何搬到远处。
    Feature moved = makePolygon(id, 9.0, 9.0, 9.1, 9.1);
    EXPECT_TRUE(store.updateFeature(moved));

    // version 递增。
    EXPECT_EQ(2u, store.getFeature(id)->version);
    // 旧位置查不到,新位置查得到(证明索引已重建)。
    EXPECT_TRUE(store.queryVisible(Rectangle(-0.5, -0.5, 0.15, 0.15)).empty());
    auto hit = store.queryVisible(Rectangle(8.5, 8.5, 9.5, 9.5));
    ASSERT_EQ(1u, hit.size());
    EXPECT_EQ(id, hit[0]);

    // 更新不存在的要素失败。
    EXPECT_FALSE(store.updateFeature(makePolygon(9999, 0, 0, 1, 1)));
}

// ============================================================
// GeoJSON 导入
// ============================================================

TEST(FeatureStoreTest, ImportGeoJsonRoundtrip) {
    FeatureStore store;
    // 北京附近两个点 + 一条线。
    const std::string geojson = R"({
        "type":"FeatureCollection","features":[
            {"type":"Feature","id":"pt-a","geometry":{"type":"Point","coordinates":[116.40,39.90]}},
            {"type":"Feature","geometry":{"type":"Point","coordinates":[116.50,39.95]}},
            {"type":"Feature","geometry":{"type":"LineString","coordinates":[[116.3,39.8],[116.6,40.0]]}}
        ]})";
    size_t n = GeoJsonImporter::importInto(geojson, store);
    EXPECT_EQ(3u, n);
    EXPECT_EQ(3u, store.size());

    // 视口覆盖北京 → 3 条全命中。
    Rectangle beijing = Rectangle::fromDegrees(116.0, 39.5, 117.0, 40.5);
    EXPECT_EQ(3u, store.queryVisible(beijing).size());

    // 远处视口(上海) → 0 命中。
    Rectangle shanghai = Rectangle::fromDegrees(121.0, 31.0, 122.0, 31.5);
    EXPECT_TRUE(store.queryVisible(shanghai).empty());

    // sourceId 保留。
    bool foundSource = false;
    for (const auto& kv : store.features())
        if (kv.second.sourceId == "pt-a") foundSource = true;
    EXPECT_TRUE(foundSource);
}

TEST(FeatureStoreTest, ImportInvalidReturnsZero) {
    FeatureStore store;
    EXPECT_EQ(0u, GeoJsonImporter::importInto("not json", store));
    EXPECT_EQ(0u, store.size());
}

TEST(FeatureStoreTest, Clear) {
    FeatureStore store;
    store.addFeature(makePolygon(kInvalidFeatureId, 0, 0, 0.1, 0.1));
    store.clear();
    EXPECT_TRUE(store.empty());
    EXPECT_TRUE(store.queryVisible(Rectangle(-10, -10, 10, 10)).empty());
}
