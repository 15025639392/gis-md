#include <gtest/gtest.h>

#include "earth_engine/core/cache/SharedAssetDepot.h"

#include <string>

using namespace earth_engine;

namespace {

struct TestAsset {
    int value = 0;
};

struct TestKeyTraits {
    static std::string toString(int key) { return std::to_string(key); }
};

using TestDepot = SharedAssetDepot<TestAsset, int, TestKeyTraits>;

} // namespace

TEST(SharedAssetDepotTest, GetMissReturnsNull) {
    TestDepot depot;
    EXPECT_EQ(nullptr, depot.get(42));
}

TEST(SharedAssetDepotTest, PutAndGet) {
    TestDepot depot;
    auto asset = std::make_shared<const TestAsset>(TestAsset{100});
    depot.put(1, asset);
    EXPECT_EQ(asset, depot.get(1));
}

TEST(SharedAssetDepotTest, Contains) {
    TestDepot depot;
    EXPECT_FALSE(depot.contains(1));
    depot.put(1, std::make_shared<const TestAsset>());
    EXPECT_TRUE(depot.contains(1));
}

TEST(SharedAssetDepotTest, Remove) {
    TestDepot depot;
    depot.put(1, std::make_shared<const TestAsset>());
    depot.remove(1);
    EXPECT_EQ(nullptr, depot.get(1));
}

TEST(SharedAssetDepotTest, InvalidateAll) {
    TestDepot depot;
    depot.put(1, std::make_shared<const TestAsset>());
    depot.put(2, std::make_shared<const TestAsset>());
    depot.invalidateAll();
    EXPECT_EQ(nullptr, depot.get(1));
    EXPECT_EQ(nullptr, depot.get(2));
}

TEST(SharedAssetDepotTest, StartInFlightFirstCallerReturnsTrue) {
    TestDepot depot;
    bool first = depot.startInFlight(1, nullptr);
    EXPECT_TRUE(first);
}

TEST(SharedAssetDepotTest, StartInFlightSecondCallerReturnsFalse) {
    TestDepot depot;
    depot.startInFlight(1, nullptr);
    bool first = depot.startInFlight(1, nullptr);
    EXPECT_FALSE(first);
}

TEST(SharedAssetDepotTest, FinishInFlightNotifiesWaiters) {
    TestDepot depot;
    int notified = 0;
    depot.startInFlight(1, [&](auto) { ++notified; });
    depot.startInFlight(1, [&](auto) { ++notified; });

    auto result = std::make_shared<const TestAsset>();
    depot.finishInFlight(1, result);
    EXPECT_EQ(2, notified);
    EXPECT_EQ(result, depot.get(1));
}

TEST(SharedAssetDepotTest, IsInFlight) {
    TestDepot depot;
    EXPECT_FALSE(depot.isInFlight(1));
    depot.startInFlight(1, nullptr);
    EXPECT_TRUE(depot.isInFlight(1));
    depot.finishInFlight(1, std::make_shared<const TestAsset>());
    EXPECT_FALSE(depot.isInFlight(1));
}

TEST(SharedAssetDepotTest, AbortInFlight) {
    TestDepot depot;
    int notified = 0;
    depot.startInFlight(1, [&](auto result) {
        ++notified;
        EXPECT_EQ(nullptr, result);
    });
    depot.abortInFlight(1);
    EXPECT_EQ(1, notified);
    EXPECT_EQ(nullptr, depot.get(1));
}

TEST(SharedAssetDepotTest, EvictsLeastRecentlyUsedOnBudget) {
    TestDepot depot;
    depot.setMaxCacheBytes(static_cast<int64_t>(3 * sizeof(TestAsset)));
    depot.put(1, std::make_shared<const TestAsset>());
    depot.put(2, std::make_shared<const TestAsset>());
    depot.put(3, std::make_shared<const TestAsset>());
    // Touch 1 so 2 becomes the LRU victim.
    EXPECT_NE(nullptr, depot.get(1));
    depot.put(4, std::make_shared<const TestAsset>());
    EXPECT_TRUE(depot.contains(1));
    EXPECT_FALSE(depot.contains(2));
    EXPECT_TRUE(depot.contains(3));
    EXPECT_TRUE(depot.contains(4));
}

// Regression: lruMap_ stores iterators into the LRU sequence; the previous
// std::deque backing invalidated them on every push_front/pop_back/erase,
// making touch()/remove() after churn undefined behavior.
TEST(SharedAssetDepotTest, LruIteratorsSurviveHeavyChurn) {
    TestDepot depot;
    depot.setMaxCacheBytes(static_cast<int64_t>(8 * sizeof(TestAsset)));
    for (int round = 0; round < 50; ++round) {
        for (int key = 0; key < 20; ++key) {
            depot.put(round * 100 + key, std::make_shared<const TestAsset>());
            // Touch an older entry between insertions.
            depot.get(round * 100 + key / 2);
        }
        // Remove a mix of present and already-evicted keys.
        depot.remove(round * 100 + 3);
        depot.remove(round * 100 + 19);
    }
    // Budget must hold and the freshest survivor must still resolve.
    EXPECT_LE(depot.cacheBytes(),
              static_cast<int64_t>(8 * sizeof(TestAsset)));
    depot.put(999999, std::make_shared<const TestAsset>());
    EXPECT_TRUE(depot.contains(999999));
}
