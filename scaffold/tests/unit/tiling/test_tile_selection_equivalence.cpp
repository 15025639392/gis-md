// §8 等价性基座单测:验证 TileSelectionEquivalence 比较器本身正确
// (identical → equal;每类字段扰动 → 被抓到并给出可读 mismatch)。
// 这是 ③ 增量切面「增量==全量」oracle 的核心组件,先独立自证。
#include "earth_engine/tiling/TileSelectionEquivalence.h"

#include <gtest/gtest.h>

using namespace earth_engine;

namespace {

TileKey key(int z, int x, int y) {
    return TileKey{"XYZ-WebMercator", z, x, y};
}

// 构造一对「相同」的选择产出。
struct Fixture {
    TilePlan plan;
    TileLoadQueue load;
    TileSelectionCounters counters;

    Fixture() {
        plan.visibleTiles = {key(1, 0, 0), key(1, 1, 0), key(2, 3, 2)};
        load.queue(key(3, 4, 4), TileLoadPriorityGroup::Normal, 10.0);
        load.queue(key(3, 5, 4), TileLoadPriorityGroup::Urgent, 5.0);
        counters.visited = 40;
        counters.culled = 12;
        counters.kicked = 3;
        counters.fogCulled = 2;
        counters.notYetRenderable = 7;
        counters.culledVisited = 1;
    }
};

TileSelectionEquivalence::Result cmp(const Fixture& a, const Fixture& b) {
    return TileSelectionEquivalence::compare(
        a.plan, a.load, a.counters, b.plan, b.load, b.counters);
}

} // namespace

TEST(TileSelectionEquivalenceTest, IdenticalPlansAreEqual) {
    Fixture a, b;
    const auto r = cmp(a, b);
    EXPECT_TRUE(r.equal) << r.mismatch;
    EXPECT_TRUE(r.mismatch.empty());
}

TEST(TileSelectionEquivalenceTest, RenderSetOrderIndependent) {
    // visibleTiles 顺序不同但集合相同 → 仍等价(不透明地形深度裁决,顺序无意义)。
    Fixture a, b;
    std::reverse(b.plan.visibleTiles.begin(), b.plan.visibleTiles.end());
    const auto r = cmp(a, b);
    EXPECT_TRUE(r.equal) << r.mismatch;
}

TEST(TileSelectionEquivalenceTest, DifferentRenderTileCaught) {
    Fixture a, b;
    b.plan.visibleTiles[2] = key(2, 3, 3);  // 换一个不同瓦片
    const auto r = cmp(a, b);
    EXPECT_FALSE(r.equal);
    EXPECT_NE(r.mismatch.find("visibleTiles"), std::string::npos);
}

TEST(TileSelectionEquivalenceTest, DifferentRenderCountCaught) {
    Fixture a, b;
    b.plan.visibleTiles.push_back(key(2, 0, 0));
    const auto r = cmp(a, b);
    EXPECT_FALSE(r.equal);
    EXPECT_NE(r.mismatch.find("visibleTiles size"), std::string::npos);
}

TEST(TileSelectionEquivalenceTest, LoadQueueOrderIndependentButContentChecked) {
    // 入队顺序不同、优先级排序后一致 → 等价。
    Fixture a;
    Fixture b;
    b.load = TileLoadQueue{};
    b.load.queue(key(3, 5, 4), TileLoadPriorityGroup::Urgent, 5.0);
    b.load.queue(key(3, 4, 4), TileLoadPriorityGroup::Normal, 10.0);
    const auto r = cmp(a, b);
    EXPECT_TRUE(r.equal) << r.mismatch;
}

TEST(TileSelectionEquivalenceTest, DifferentLoadPriorityCaught) {
    Fixture a, b;
    b.load = TileLoadQueue{};
    b.load.queue(key(3, 4, 4), TileLoadPriorityGroup::Normal, 99.0);  // 优先级变
    b.load.queue(key(3, 5, 4), TileLoadPriorityGroup::Urgent, 5.0);
    const auto r = cmp(a, b);
    EXPECT_FALSE(r.equal);
    EXPECT_NE(r.mismatch.find("loadQueue"), std::string::npos);
}

TEST(TileSelectionEquivalenceTest, DifferentCounterCaught) {
    Fixture a, b;
    b.counters.kicked = 4;  // 3 → 4
    const auto r = cmp(a, b);
    EXPECT_FALSE(r.equal);
    EXPECT_NE(r.mismatch.find("kicked"), std::string::npos);
}

TEST(TileSelectionEquivalenceTest, DifferentNotYetRenderableCaught) {
    Fixture a, b;
    b.counters.notYetRenderable = 8;
    const auto r = cmp(a, b);
    EXPECT_FALSE(r.equal);
    EXPECT_NE(r.mismatch.find("notYetRenderable"), std::string::npos);
}
