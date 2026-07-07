#include <gtest/gtest.h>

#include "earth_engine/data/FeatureBucketGrid.h"
#include "earth_engine/data/FeatureStore.h"
#include "earth_engine/core/geodesy/Cartographic.h"
#include "earth_engine/core/math/Rectangle.h"

#include <set>

using namespace earth_engine;

namespace {

// 轴对齐小矩形要素(bounds 直接给)。
Feature polyWithBounds(FeatureId id, double w, double s, double e, double n) {
    Feature f;
    f.id = id;
    f.type = GeometryType::Polygon;
    f.rings = {{Cartographic(w, s), Cartographic(e, s), Cartographic(e, n),
                Cartographic(w, n), Cartographic(w, s)}};
    return f;
}

} // namespace

// ============================================================
// key 打包
// ============================================================

TEST(FeatureBucketGridTest, PackUnpackRoundtripIncludingNegatives) {
    for (int32_t cx : {-1000, -1, 0, 1, 1000}) {
        for (int32_t cy : {-777, -1, 0, 1, 777}) {
            BucketKey k = FeatureBucketGrid::packCell(cx, cy);
            EXPECT_EQ(cx, FeatureBucketGrid::cellX(k));
            EXPECT_EQ(cy, FeatureBucketGrid::cellY(k));
        }
    }
}

// ============================================================
// bucketFor:按中心 + 超大
// ============================================================

TEST(FeatureBucketGridTest, BucketForByCenter) {
    FeatureBucketGrid g(0.1);
    // 中心 (0.03,0.03) → cell (0,0)
    EXPECT_EQ(FeatureBucketGrid::packCell(0, 0),
              g.bucketFor(Rectangle(0.02, 0.02, 0.04, 0.04)));
    // 中心 (0.35,0.35) → cell (3,3)
    EXPECT_EQ(FeatureBucketGrid::packCell(3, 3),
              g.bucketFor(Rectangle(0.34, 0.34, 0.36, 0.36)));
    // 负坐标:中心 (-0.05,-0.05) → floor(-0.5)=-1
    EXPECT_EQ(FeatureBucketGrid::packCell(-1, -1),
              g.bucketFor(Rectangle(-0.06, -0.06, -0.04, -0.04)));
}

TEST(FeatureBucketGridTest, OversizedFeatureGoesToOversizedBucket) {
    FeatureBucketGrid g(0.1);
    // 宽 0.2 > cell 0.1 → 超大
    EXPECT_EQ(FeatureBucketGrid::kOversizedBucket,
              g.bucketFor(Rectangle(0.0, 0.0, 0.2, 0.05)));
    // 高 0.3 > cell → 超大
    EXPECT_EQ(FeatureBucketGrid::kOversizedBucket,
              g.bucketFor(Rectangle(0.0, 0.0, 0.05, 0.3)));
}

// ============================================================
// assign / unassign / dirty
// ============================================================

TEST(FeatureBucketGridTest, AssignPopulatesMembershipAndDirty) {
    FeatureBucketGrid g(0.1);
    BucketKey k1 = g.assign(1, Rectangle(0.01, 0.01, 0.02, 0.02));
    BucketKey k2 = g.assign(2, Rectangle(0.03, 0.03, 0.04, 0.04));  // 同 cell(0,0)
    EXPECT_EQ(k1, k2);
    EXPECT_EQ(1u, g.bucketCount());

    const auto* members = g.featuresIn(k1);
    ASSERT_NE(nullptr, members);
    EXPECT_EQ(2u, members->size());
    EXPECT_TRUE(members->count(1));
    EXPECT_TRUE(members->count(2));

    auto dirty = g.consumeDirty();
    EXPECT_EQ(1u, dirty.size());
    EXPECT_TRUE(dirty.count(k1));
    EXPECT_TRUE(g.dirtyBuckets().empty());  // consume 后清空
}

TEST(FeatureBucketGridTest, UnassignEmptyRemovesBucketButMarksDirty) {
    FeatureBucketGrid g(0.1);
    BucketKey k = g.assign(1, Rectangle(0.01, 0.01, 0.02, 0.02));
    g.consumeDirty();  // 清掉 assign 的脏

    g.unassign(1, Rectangle(0.01, 0.01, 0.02, 0.02));
    EXPECT_EQ(0u, g.bucketCount());          // 空桶移除
    EXPECT_EQ(nullptr, g.featuresIn(k));
    auto dirty = g.consumeDirty();
    EXPECT_TRUE(dirty.count(k));             // 仍标脏 → consumer 丢弃几何
}

TEST(FeatureBucketGridTest, UnassignOneOfManyKeepsBucket) {
    FeatureBucketGrid g(0.1);
    BucketKey k = g.assign(1, Rectangle(0.01, 0.01, 0.02, 0.02));
    g.assign(2, Rectangle(0.03, 0.03, 0.04, 0.04));  // 同 cell
    g.consumeDirty();

    g.unassign(1, Rectangle(0.01, 0.01, 0.02, 0.02));
    const auto* members = g.featuresIn(k);
    ASSERT_NE(nullptr, members);
    EXPECT_EQ(1u, members->size());
    EXPECT_TRUE(members->count(2));
}

// ============================================================
// bucketsInView + cellRect
// ============================================================

TEST(FeatureBucketGridTest, CellRect) {
    FeatureBucketGrid g(0.1);
    Rectangle r = g.cellRect(FeatureBucketGrid::packCell(2, -3));
    EXPECT_NEAR(0.2, r.west(), 1e-12);
    EXPECT_NEAR(-0.3, r.south(), 1e-12);
    EXPECT_NEAR(0.3, r.east(), 1e-12);
    EXPECT_NEAR(-0.2, r.north(), 1e-12);
}

TEST(FeatureBucketGridTest, BucketsInViewSelectsIntersecting) {
    FeatureBucketGrid g(0.1);
    g.assign(1, Rectangle(0.05, 0.05, 0.06, 0.06));  // cell (0,0)
    g.assign(2, Rectangle(0.55, 0.55, 0.56, 0.56));  // cell (5,5),远
    g.assign(3, Rectangle(0.0, 0.0, 0.3, 0.05));     // 超大(宽0.3)

    // 视口覆盖原点附近 → cell(0,0) + 超大桶,不含 cell(5,5)
    auto keys = g.bucketsInView(Rectangle(-0.02, -0.02, 0.08, 0.08));
    std::set<BucketKey> got(keys.begin(), keys.end());
    EXPECT_TRUE(got.count(FeatureBucketGrid::packCell(0, 0)));
    EXPECT_TRUE(got.count(FeatureBucketGrid::kOversizedBucket));
    EXPECT_FALSE(got.count(FeatureBucketGrid::packCell(5, 5)));
}

TEST(FeatureBucketGridTest, OversizedAlwaysInViewEvenWhenViewportElsewhere) {
    FeatureBucketGrid g(0.1);
    g.assign(1, Rectangle(0.0, 0.0, 0.5, 0.05));  // 超大,中心 cell(2,0)
    // 视口在别处(不覆盖中心 cell),超大桶仍纳入。
    auto keys = g.bucketsInView(Rectangle(10.0, 10.0, 10.1, 10.1));
    ASSERT_EQ(1u, keys.size());
    EXPECT_EQ(FeatureBucketGrid::kOversizedBucket, keys[0]);
}

// ============================================================
// FeatureStore 集成:编辑自动维护桶 + 脏区
// ============================================================

TEST(FeatureBucketGridTest, StoreAddPopulatesBucketsAndDirty) {
    FeatureStore store;  // 默认 cell 0.02
    FeatureId a = store.addFeature(polyWithBounds(0, 0.0, 0.0, 0.005, 0.005));
    FeatureId b = store.addFeature(polyWithBounds(0, 1.0, 1.0, 1.005, 1.005));

    // 两要素相距远 → 不同桶,均标脏。
    EXPECT_GE(store.dirtyBuckets().size(), 2u);

    // 视口覆盖 a → 含 a 所在桶且能取到 a。
    auto keys = store.bucketsInView(Rectangle(-0.1, -0.1, 0.1, 0.1));
    bool foundA = false;
    for (BucketKey k : keys) {
        const auto* members = store.featuresInBucket(k);
        if (members && members->count(a)) foundA = true;
    }
    EXPECT_TRUE(foundA);
    (void)b;
}

TEST(FeatureBucketGridTest, StoreUpdateAcrossBucketsMarksBothDirty) {
    FeatureStore store;
    FeatureId id = store.addFeature(polyWithBounds(0, 0.0, 0.0, 0.005, 0.005));
    store.consumeDirtyBuckets();  // 清掉 add 的脏

    // 把要素搬到远处的另一个桶。
    Feature moved = polyWithBounds(id, 5.0, 5.0, 5.005, 5.005);
    ASSERT_TRUE(store.updateFeature(moved));

    // 旧桶 + 新桶都应标脏(≥2)。
    auto dirty = store.consumeDirtyBuckets();
    EXPECT_GE(dirty.size(), 2u);

    // 新位置视口能取到,旧位置取不到。
    bool foundNew = false;
    for (BucketKey k : store.bucketsInView(Rectangle(4.9, 4.9, 5.1, 5.1))) {
        const auto* m = store.featuresInBucket(k);
        if (m && m->count(id)) foundNew = true;
    }
    EXPECT_TRUE(foundNew);
}

TEST(FeatureBucketGridTest, StoreRemoveMarksDirtyAndDropsFromBucket) {
    FeatureStore store;
    FeatureId id = store.addFeature(polyWithBounds(0, 0.0, 0.0, 0.005, 0.005));
    store.consumeDirtyBuckets();

    ASSERT_TRUE(store.removeFeature(id));
    auto dirty = store.consumeDirtyBuckets();
    EXPECT_FALSE(dirty.empty());

    // 任何桶都不再含该要素。
    for (BucketKey k : store.bucketsInView(Rectangle(-1, -1, 1, 1))) {
        const auto* m = store.featuresInBucket(k);
        if (m) EXPECT_FALSE(m->count(id));
    }
}
