#include <gtest/gtest.h>

#include "earth_engine/content/GltfContentProvider.h"
#include "earth_engine/core/resources/FrameResourceBudget.h"
#include "earth_engine/providers/TerrainProvider.h"
#include "earth_engine/tiling/RasterMappedToTilesetTile.h"
#include "earth_engine/tiling/TileLoadLifecycle.h"
#include "earth_engine/tiling/TileLoadScheduler.h"
#include "earth_engine/tiling/TilesetTile.h"

#include <mutex>
#include <string>

using namespace earth_engine;

namespace {

std::string cacheKeyForTile(const TileKey& key) {
    return key.schemeId + ":" +
           std::to_string(key.z) + ":" +
           std::to_string(key.x) + ":" +
           std::to_string(key.y);
}

class CountingContentProvider final : public TilesetContentProvider {
public:
    std::string id() const override { return "scheduler-content-inflight"; }
    bool supportsTile(const TileKey&) const override { return true; }
    void requestTileContent(
        const TileKey&,
        CancellationToken,
        ContentCallback,
        HttpRequestPriority = HttpRequestPriority::Normal) override {
        ++requestCount;
    }
    TileContentLoadResult decodeContent(const uint8_t*, size_t) override {
        return TileContentLoadResult::failed();
    }

    int requestCount = 0;
};

class FanoutTerrainProvider final : public TerrainProvider {
public:
    std::string id() const override { return "scheduler-fanout-terrain"; }
    std::string schemeId() const override { return "test"; }
    int minZoom() const override { return 0; }
    int maxZoom() const override { return 1; }
    int tileSize() const override { return 2; }
    std::string buildUrl(const TileKey&) const override {
        return "memory://scheduler-fanout-terrain";
    }
    int estimatedRequestFanout(const TileKey&) const override { return 2; }
    void requestTile(
        const TileKey&,
        CancellationToken,
        HeightmapCallback,
        HttpRequestPriority = HttpRequestPriority::Normal) override {
        ++requestCount;
    }
    std::unique_ptr<DecodedHeightmap> decodeTile(const uint8_t*, size_t)
        override {
        return nullptr;
    }

    int requestCount = 0;
};

} // namespace

TEST(TileLoadSchedulerTest, BlocksContentRequestWhenInflightIsFull) {
    TileLoadLifecycle lifecycle;
    FrameResourceBudgetConfig config;
    config.maxNetworkInflight = 1;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);
    CancellationToken token;
    {
        std::lock_guard<std::mutex> lock(lifecycle.mutex());
        ASSERT_TRUE(lifecycle.requestState().beginTerrainRequest(
            "busy",
            token));
    }

    bool planned = false;
    bool marked = false;
    CountingContentProvider provider;

    const TileLoadRequestOutcome outcome =
        TileLoadScheduler::requestMissingTiles(
            {TileLoadRequest{
                TileKey{"test", 0, 0, 0},
                TileLoadPriorityGroup::Normal,
                0.0}},
            TileLoadSchedulerInput{
                lifecycle,
                budget,
                nullptr,
                &provider},
            cacheKeyForTile,
            [&planned](
                const TileKey&,
                const std::string&,
                TilesetTile*& tileState) {
                planned = true;
                tileState = nullptr;
                TileLoadRequestSnapshot snapshot;
                snapshot.contentProviderSupportsTile = true;
                return snapshot;
            },
            [](const std::string&) { return false; },
            [](TilesetTile&, double) { return false; },
            [&marked](const TileKey&) { marked = true; });

    EXPECT_EQ(outcome.issued, 0u);
    EXPECT_TRUE(outcome.blockedByInflight);
    EXPECT_TRUE(planned);
    EXPECT_FALSE(marked);
    EXPECT_EQ(provider.requestCount, 0);

    {
        std::lock_guard<std::mutex> lock(lifecycle.mutex());
        lifecycle.requestState().completeTerrainRequest("busy");
    }
}

TEST(TileLoadSchedulerTest, SkipsPendingCacheKeyBeforeInflightBlock) {
    TileLoadLifecycle lifecycle;
    FrameResourceBudgetConfig config;
    config.maxNetworkInflight = 1;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);
    const TileKey key{"test", 0, 0, 0};
    const std::string cacheKey = cacheKeyForTile(key);
    CancellationToken token;
    {
        std::lock_guard<std::mutex> lock(lifecycle.mutex());
        ASSERT_TRUE(lifecycle.requestState().beginTerrainRequest(
            cacheKey,
            token));
    }

    bool planned = false;

    const TileLoadRequestOutcome outcome =
        TileLoadScheduler::requestMissingTiles(
            {TileLoadRequest{
                key,
                TileLoadPriorityGroup::Urgent,
                100.0}},
            TileLoadSchedulerInput{
                lifecycle,
                budget,
                nullptr,
                nullptr},
            cacheKeyForTile,
            [&planned](
                const TileKey&,
                const std::string&,
                TilesetTile*& tileState) {
                planned = true;
                tileState = nullptr;
                return TileLoadRequestSnapshot{};
            },
            [](const std::string&) { return false; },
            [](TilesetTile&, double) { return false; },
            [](const TileKey&) {});

    EXPECT_EQ(outcome.issued, 0u);
    EXPECT_FALSE(outcome.blockedByInflight);
    EXPECT_FALSE(planned);

    {
        std::lock_guard<std::mutex> lock(lifecycle.mutex());
        lifecycle.requestState().completeTerrainRequest(cacheKey);
    }
}

TEST(TileLoadSchedulerTest, SkipsEmptyCacheKeyBeforeInflightBlock) {
    TileLoadLifecycle lifecycle;
    FrameResourceBudgetConfig config;
    config.maxNetworkInflight = 1;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);
    CancellationToken token;
    {
        std::lock_guard<std::mutex> lock(lifecycle.mutex());
        ASSERT_TRUE(lifecycle.requestState().beginTerrainRequest(
            "busy",
            token));
    }

    bool planned = false;

    const TileLoadRequestOutcome outcome =
        TileLoadScheduler::requestMissingTiles(
            {TileLoadRequest{
                TileKey{"test", 0, 0, 0},
                TileLoadPriorityGroup::Urgent,
                100.0}},
            TileLoadSchedulerInput{
                lifecycle,
                budget,
                nullptr,
                nullptr},
            [](const TileKey&) { return std::string{}; },
            [&planned](
                const TileKey&,
                const std::string&,
                TilesetTile*& tileState) {
                planned = true;
                tileState = nullptr;
                return TileLoadRequestSnapshot{};
            },
            [](const std::string&) { return false; },
            [](TilesetTile&, double) { return false; },
            [](const TileKey&) {});

    EXPECT_EQ(outcome.issued, 0u);
    EXPECT_FALSE(outcome.blockedByInflight);
    EXPECT_FALSE(planned);
    EXPECT_EQ(lifecycle.pendingRequestCount(), 1u);

    {
        std::lock_guard<std::mutex> lock(lifecycle.mutex());
        lifecycle.requestState().completeTerrainRequest("busy");
    }
}

TEST(TileLoadSchedulerTest, SkipsKnownEmptyContentBeforeInflightBlock) {
    TileLoadLifecycle lifecycle;
    FrameResourceBudgetConfig config;
    config.maxNetworkInflight = 1;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);
    CancellationToken token;
    {
        std::lock_guard<std::mutex> lock(lifecycle.mutex());
        ASSERT_TRUE(lifecycle.requestState().beginTerrainRequest(
            "busy",
            token));
    }

    const TileKey key{"test", 0, 0, 0};
    const std::string cacheKey = cacheKeyForTile(key);
    bool planned = false;

    const TileLoadRequestOutcome outcome =
        TileLoadScheduler::requestMissingTiles(
            {TileLoadRequest{
                key,
                TileLoadPriorityGroup::Urgent,
                100.0}},
            TileLoadSchedulerInput{
                lifecycle,
                budget,
                nullptr,
                nullptr},
            cacheKeyForTile,
            [&planned](
                const TileKey&,
                const std::string&,
                TilesetTile*& tileState) {
                planned = true;
                tileState = nullptr;
                return TileLoadRequestSnapshot{};
            },
            [&cacheKey](const std::string& keyToCheck) {
                return keyToCheck == cacheKey;
            },
            [](TilesetTile&, double) { return false; },
            [](const TileKey&) {});

    EXPECT_EQ(outcome.issued, 0u);
    EXPECT_FALSE(outcome.blockedByInflight);
    EXPECT_FALSE(planned);
    EXPECT_EQ(lifecycle.pendingRequestCount(), 1u);

    {
        std::lock_guard<std::mutex> lock(lifecycle.mutex());
        lifecycle.requestState().completeTerrainRequest("busy");
    }
}

TEST(TileLoadSchedulerTest, BlocksTerrainFanoutOverInflightCapacity) {
    TileLoadLifecycle lifecycle;
    FrameResourceBudgetConfig config;
    config.maxNetworkInflight = 2;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);
    CancellationToken token;
    {
        std::lock_guard<std::mutex> lock(lifecycle.mutex());
        ASSERT_TRUE(lifecycle.requestState().beginTerrainRequest(
            "busy",
            token));
    }

    const TileKey key{"test", 0, 0, 0};
    FanoutTerrainProvider provider;
    bool marked = false;

    const TileLoadRequestOutcome outcome =
        TileLoadScheduler::requestMissingTiles(
            {TileLoadRequest{
                key,
                TileLoadPriorityGroup::Urgent,
                100.0}},
            TileLoadSchedulerInput{
                lifecycle,
                budget,
                &provider,
                nullptr},
            cacheKeyForTile,
            [](const TileKey&,
               const std::string&,
               TilesetTile*& tileState) {
                tileState = nullptr;
                TileLoadRequestSnapshot snapshot;
                snapshot.terrainProviderSupportsTile = true;
                return snapshot;
            },
            [](const std::string&) { return false; },
            [](TilesetTile&, double) { return false; },
            [&marked](const TileKey&) { marked = true; });

    EXPECT_EQ(outcome.issued, 0u);
    EXPECT_TRUE(outcome.blockedByInflight);
    EXPECT_EQ(provider.requestCount, 0);
    EXPECT_FALSE(marked);
    EXPECT_EQ(lifecycle.pendingRequestCount(), 1u);

    {
        std::lock_guard<std::mutex> lock(lifecycle.mutex());
        lifecycle.requestState().completeTerrainRequest("busy");
    }
}

TEST(TileLoadSchedulerTest, QueuesUpsampledTerrainWhenNetworkInflightIsFull) {
    TileLoadLifecycle lifecycle;
    FrameResourceBudgetConfig config;
    config.maxNetworkRequestsPerFrame = 4;
    config.maxNetworkInflight = 1;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);
    CancellationToken token;
    {
        std::lock_guard<std::mutex> lock(lifecycle.mutex());
        ASSERT_TRUE(lifecycle.requestState().beginTerrainRequest(
            "busy",
            token));
    }

    const TileKey key{"test", 1, 0, 0};
    TilesetTile tile(key, Rectangle{});
    tile.content.upsampledFromParent = true;
    bool prepared = false;
    bool marked = false;

    const TileLoadRequestOutcome outcome =
        TileLoadScheduler::requestMissingTiles(
            {TileLoadRequest{
                key,
                TileLoadPriorityGroup::Urgent,
                100.0}},
            TileLoadSchedulerInput{
                lifecycle,
                budget,
                nullptr,
                nullptr},
            cacheKeyForTile,
            [&tile](
                const TileKey&,
                const std::string&,
                TilesetTile*& tileState) {
                tileState = &tile;
                TileLoadRequestSnapshot snapshot;
                snapshot.hasTile = true;
                snapshot.upsampledFromParent = true;
                return snapshot;
            },
            [](const std::string&) { return false; },
            [&prepared](TilesetTile&, double) {
                prepared = true;
                return true;
            },
            [&marked](const TileKey&) { marked = true; });

    EXPECT_EQ(outcome.issued, 1u);
    EXPECT_FALSE(outcome.blockedByInflight);
    EXPECT_TRUE(prepared);
    EXPECT_TRUE(marked);
    EXPECT_EQ(lifecycle.counts().terrainUploads, 1u);
    EXPECT_EQ(lifecycle.pendingRequestCount(), 1u);

    {
        std::lock_guard<std::mutex> lock(lifecycle.mutex());
        lifecycle.requestState().completeTerrainRequest("busy");
        lifecycle.pendingLoads().clear();
    }
}

TEST(TileLoadSchedulerTest, SkipsCachedTerrainWhenNetworkInflightIsFull) {
    TileLoadLifecycle lifecycle;
    FrameResourceBudgetConfig config;
    config.maxNetworkInflight = 1;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);
    CancellationToken token;
    {
        std::lock_guard<std::mutex> lock(lifecycle.mutex());
        ASSERT_TRUE(lifecycle.requestState().beginTerrainRequest(
            "busy",
            token));
    }

    const TileKey key{"test", 1, 0, 0};
    bool planned = false;
    bool marked = false;

    const TileLoadRequestOutcome outcome =
        TileLoadScheduler::requestMissingTiles(
            {TileLoadRequest{
                key,
                TileLoadPriorityGroup::Urgent,
                100.0}},
            TileLoadSchedulerInput{
                lifecycle,
                budget,
                nullptr,
                nullptr},
            cacheKeyForTile,
            [&planned](
                const TileKey&,
                const std::string&,
                TilesetTile*& tileState) {
                planned = true;
                tileState = nullptr;
                TileLoadRequestSnapshot snapshot;
                snapshot.terrainProviderSupportsTile = true;
                snapshot.terrainAlreadyCached = true;
                return snapshot;
            },
            [](const std::string&) { return false; },
            [](TilesetTile&, double) { return false; },
            [&marked](const TileKey&) { marked = true; });

    EXPECT_EQ(outcome.issued, 0u);
    EXPECT_FALSE(outcome.blockedByInflight);
    EXPECT_TRUE(planned);
    EXPECT_FALSE(marked);
    EXPECT_EQ(lifecycle.pendingRequestCount(), 1u);

    {
        std::lock_guard<std::mutex> lock(lifecycle.mutex());
        lifecycle.requestState().completeTerrainRequest("busy");
    }
}
