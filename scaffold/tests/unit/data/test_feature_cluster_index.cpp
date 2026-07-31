#include <gtest/gtest.h>

#include "earth_engine/data/FeatureClusterIndex.h"
#include "earth_engine/data/FeatureStore.h"

#include <cmath>
#include <set>

using namespace earth_engine;

namespace {

constexpr double kDeg = M_PI / 180.0;

FeatureId addPoint(FeatureStore& store, double lonDeg, double latDeg) {
    Feature p;
    p.type = GeometryType::Point;
    p.rings = {{Cartographic(lonDeg * kDeg, latDeg * kDeg)}};
    return store.addFeature(std::move(p));
}

Rectangle worldBbox() {
    return Rectangle(-M_PI, -M_PI / 2.0, M_PI, M_PI / 2.0);
}

FeatureClusterOptions defaultOptions() {
    FeatureClusterOptions o;
    o.minZoom = 0;
    o.maxZoom = 16;
    o.radiusPx = 60.0;
    return o;
}

} // namespace

TEST(FeatureClusterIndexTest, EmptyStoreYieldsEmptyIndex) {
    FeatureStore store;
    FeatureClusterIndex index;
    index.build(store, defaultOptions());
    EXPECT_TRUE(index.empty());
    EXPECT_EQ(0u, index.levelCount());
    EXPECT_TRUE(index.query(worldBbox(), 5.0).empty());
}

TEST(FeatureClusterIndexTest, NonPointFeaturesIgnored) {
    FeatureStore store;
    Feature line;
    line.type = GeometryType::LineString;
    line.rings = {{Cartographic(0.0, 0.0), Cartographic(0.01, 0.0)}};
    store.addFeature(std::move(line));
    FeatureClusterIndex index;
    index.build(store, defaultOptions());
    EXPECT_TRUE(index.empty());
}

TEST(FeatureClusterIndexTest, NearbyPointsMergeWhenZoomedOutSplitWhenZoomedIn) {
    // 5 个点挤在 ~200m 内:粗 zoom 必然聚成 1 簇,最细 zoom 全展开。
    FeatureStore store;
    for (int i = 0; i < 5; ++i) {
        addPoint(store, 106.0 + i * 0.001, 29.0);
    }
    FeatureClusterIndex index;
    index.build(store, defaultOptions());
    ASSERT_FALSE(index.empty());

    const auto coarse = index.query(worldBbox(), 3.0);
    ASSERT_EQ(1u, coarse.size());
    EXPECT_TRUE(coarse[0].isCluster());
    EXPECT_EQ(5u, coarse[0].count);
    EXPECT_EQ(kInvalidFeatureId, coarse[0].featureId);
    // 代表点落在成员之间。
    EXPECT_NEAR(106.002 * kDeg, coarse[0].longitude, 1e-9);

    const auto fine = index.query(worldBbox(), 16.0);
    EXPECT_EQ(5u, fine.size());
    for (const auto& c : fine) {
        EXPECT_FALSE(c.isCluster());
        EXPECT_NE(kInvalidFeatureId, c.featureId);
    }
}

TEST(FeatureClusterIndexTest, FarApartPointsNeverMerge) {
    // 三个相隔上千公里的点:即使 zoom 0 也各自独立(超出 60px 半径)。
    FeatureStore store;
    addPoint(store, -100.0, 40.0);
    addPoint(store, 10.0, 0.0);
    addPoint(store, 120.0, -30.0);
    FeatureClusterIndex index;
    index.build(store, defaultOptions());

    const auto clusters = index.query(worldBbox(), 0.0);
    ASSERT_EQ(3u, clusters.size());
    for (const auto& c : clusters) EXPECT_EQ(1u, c.count);
}

TEST(FeatureClusterIndexTest, ClusterCountsAreConservedAcrossZoomLevels) {
    FeatureStore store;
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            addPoint(store, 106.0 + i * 0.02, 29.0 + j * 0.02);
        }
    }
    FeatureClusterIndex index;
    index.build(store, defaultOptions());

    // 任意 zoom 下,所有条目 count 之和恒 = 要素总数(不丢点、不重复计)。
    for (double zoom = 0.0; zoom <= 16.0; zoom += 1.0) {
        uint32_t total = 0;
        for (const auto& c : index.query(worldBbox(), zoom)) total += c.count;
        EXPECT_EQ(16u, total) << "zoom=" << zoom;
    }
}

TEST(FeatureClusterIndexTest, ViewportQueryFiltersByRepresentativePoint) {
    FeatureStore store;
    addPoint(store, 106.0, 29.0);
    addPoint(store, -60.0, -20.0);
    FeatureClusterIndex index;
    index.build(store, defaultOptions());

    const Rectangle asia(100.0 * kDeg, 20.0 * kDeg, 110.0 * kDeg,
                         35.0 * kDeg);
    const auto clusters = index.query(asia, 8.0);
    ASSERT_EQ(1u, clusters.size());
    EXPECT_NEAR(106.0 * kDeg, clusters[0].longitude, 1e-9);
}

TEST(FeatureClusterIndexTest, LeavesReturnAllMembersOfCluster) {
    FeatureStore store;
    std::set<FeatureId> ids;
    for (int i = 0; i < 6; ++i) {
        ids.insert(addPoint(store, 106.0 + i * 0.001, 29.0));
    }
    FeatureClusterIndex index;
    index.build(store, defaultOptions());

    const auto coarse = index.query(worldBbox(), 2.0);
    ASSERT_EQ(1u, coarse.size());
    const auto leaves = index.leaves(coarse[0].clusterId);
    ASSERT_EQ(ids.size(), leaves.size());
    EXPECT_EQ(ids, std::set<FeatureId>(leaves.begin(), leaves.end()));
}

TEST(FeatureClusterIndexTest, LeavesOfSinglePointIsItself) {
    FeatureStore store;
    const FeatureId id = addPoint(store, 106.0, 29.0);
    FeatureClusterIndex index;
    index.build(store, defaultOptions());

    const auto clusters = index.query(worldBbox(), 16.0);
    ASSERT_EQ(1u, clusters.size());
    const auto leaves = index.leaves(clusters[0].clusterId);
    ASSERT_EQ(1u, leaves.size());
    EXPECT_EQ(id, leaves[0]);
}

TEST(FeatureClusterIndexTest, ExpansionZoomActuallySplitsTheCluster) {
    FeatureStore store;
    for (int i = 0; i < 8; ++i) addPoint(store, 106.0 + i * 0.002, 29.0);
    FeatureClusterIndex index;
    index.build(store, defaultOptions());

    const auto coarse = index.query(worldBbox(), 1.0);
    ASSERT_EQ(1u, coarse.size());
    ASSERT_TRUE(coarse[0].isCluster());
    const int expansion = index.expansionZoom(coarse[0].clusterId);
    EXPECT_GT(expansion, 1);
    EXPECT_LE(expansion, index.options().maxZoom);
    // 契约:到 expansionZoom 这一级,该簇范围内条目数 > 1。
    const auto after = index.query(worldBbox(), expansion);
    EXPECT_GT(after.size(), 1u);
    // 前一级仍是单条目(否则 expansionZoom 给早了)。
    const auto before = index.query(worldBbox(), expansion - 1);
    EXPECT_EQ(1u, before.size());
}

TEST(FeatureClusterIndexTest, ExpansionZoomOfSinglePointIsBeyondMaxZoom) {
    FeatureStore store;
    addPoint(store, 106.0, 29.0);
    FeatureClusterIndex index;
    index.build(store, defaultOptions());
    const auto clusters = index.query(worldBbox(), 10.0);
    ASSERT_EQ(1u, clusters.size());
    EXPECT_EQ(index.options().maxZoom + 1,
              index.expansionZoom(clusters[0].clusterId));
}

TEST(FeatureClusterIndexTest, UnknownClusterIdIsHandledGracefully) {
    FeatureStore store;
    addPoint(store, 106.0, 29.0);
    FeatureClusterIndex index;
    index.build(store, defaultOptions());
    EXPECT_TRUE(index.leaves(0xDEADBEEFDEADBEEFull).empty());
    EXPECT_EQ(index.options().maxZoom + 1,
              index.expansionZoom(0xDEADBEEFDEADBEEFull));
}

TEST(FeatureClusterIndexTest, MinPointsKeepsSmallGroupsUnclustered) {
    // 两点相邻,minPoints=3 → 不成簇,各自上浮为单点。
    FeatureStore store;
    addPoint(store, 106.0, 29.0);
    addPoint(store, 106.0005, 29.0);
    FeatureClusterOptions opts = defaultOptions();
    opts.minPoints = 3;
    FeatureClusterIndex index;
    index.build(store, opts);

    const auto clusters = index.query(worldBbox(), 0.0);
    ASSERT_EQ(2u, clusters.size());
    for (const auto& c : clusters) EXPECT_EQ(1u, c.count);
}

TEST(FeatureClusterIndexTest, RebuildAfterEditReflectsNewFeatures) {
    // 索引不订阅 store 变更:编辑后需重建(契约,应用层负责调用)。
    FeatureStore store;
    addPoint(store, 106.0, 29.0);
    FeatureClusterIndex index;
    index.build(store, defaultOptions());
    EXPECT_EQ(1u, index.query(worldBbox(), 16.0).size());

    addPoint(store, 120.0, 10.0);
    EXPECT_EQ(1u, index.query(worldBbox(), 16.0).size());  // 旧索引不变
    index.build(store, defaultOptions());
    EXPECT_EQ(2u, index.query(worldBbox(), 16.0).size());
}

TEST(FeatureClusterIndexTest, LargeScaleBuildStaysConsistent) {
    // 万级规模:count 守恒 + 层数正确(性能不在单测断言,只验不崩不丢)。
    FeatureStore store;
    for (int i = 0; i < 10000; ++i) {
        const double lon = 100.0 + (i % 100) * 0.05;
        const double lat = 20.0 + (i / 100) * 0.05;
        addPoint(store, lon, lat);
    }
    FeatureClusterIndex index;
    index.build(store, defaultOptions());
    EXPECT_EQ(17u, index.levelCount());  // maxZoom 16 → 0

    uint32_t total = 0;
    for (const auto& c : index.query(worldBbox(), 4.0)) total += c.count;
    EXPECT_EQ(10000u, total);
}
