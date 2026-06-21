#include <gtest/gtest.h>

#include "earth_engine/content/GltfContentProvider.h"
#include "earth_engine/providers/TerrainProvider.h"
#include "earth_engine/tiling/TileLoadRequestDispatcher.h"

#include <condition_variable>
#include <mutex>

using namespace earth_engine;

class DispatcherBudgetTerrainProvider final : public TerrainProvider {
public:
    std::string id() const override { return "dispatcher-budget"; }
    std::string schemeId() const override { return "test"; }
    int minZoom() const override { return 0; }
    int maxZoom() const override { return 1; }
    int tileSize() const override { return 2; }
    std::string buildUrl(const TileKey&) const override {
        return "memory://dispatcher-budget";
    }
    void requestTile(
        const TileKey&,
        CancellationToken,
        HeightmapCallback,
        HttpRequestPriority = HttpRequestPriority::Normal) override {
        ++requestCount;
    }
    std::unique_ptr<DecodedHeightmap> decodeTile(
        const uint8_t*,
        size_t) override {
        return nullptr;
    }

    int requestCount = 0;
};

class DispatcherBudgetContentProvider final : public TilesetContentProvider {
public:
    std::string id() const override { return "dispatcher-budget-content"; }
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

TEST(TileLoadRequestDispatcherTest, BlocksWhenBudgetIsExhausted) {
    std::mutex mutex;
    std::condition_variable condition;
    TilePendingRequestState requestState;
    TilePendingLoadQueue pendingLoads;
    FrameResourceBudgetConfig config;
    config.maxNetworkRequestsPerFrame = 0;
    const TileKey key{"test", 0, 0, 0};

    FrameResourceBudget terrainBudget;
    terrainBudget.beginFrame(1, config);
    DispatcherBudgetTerrainProvider terrainProvider;
    bool terrainIssued = false;

    TileLoadDispatchResult terrainResult =
        TileLoadRequestDispatcher::requestTerrain(
            mutex,
            condition,
            requestState,
            pendingLoads,
            terrainBudget,
            terrainProvider,
            key,
            "blocked",
            TileLoadPriorityGroup::Normal,
            0.0,
            [&terrainIssued]() { terrainIssued = true; });

    EXPECT_EQ(TileLoadDispatchResult::Blocked, terrainResult);
    EXPECT_FALSE(terrainIssued);
    EXPECT_EQ(0, terrainProvider.requestCount);
    EXPECT_TRUE(requestState.empty());

    FrameResourceBudget contentBudget;
    contentBudget.beginFrame(1, config);
    DispatcherBudgetContentProvider contentProvider;
    bool contentIssued = false;

    TileLoadDispatchResult contentResult =
        TileLoadRequestDispatcher::requestContent(
            mutex,
            condition,
            requestState,
            pendingLoads,
            contentBudget,
            contentProvider,
            key,
            "blocked-content",
            TileLoadPriorityGroup::Normal,
            0.0,
            [&contentIssued]() { contentIssued = true; });

    EXPECT_EQ(TileLoadDispatchResult::Blocked, contentResult);
    EXPECT_FALSE(contentIssued);
    EXPECT_EQ(0, contentProvider.requestCount);
    EXPECT_TRUE(requestState.empty());
    EXPECT_FALSE(pendingLoads.hasWork());
    EXPECT_EQ(0u, contentBudget.networkRequestsIssued());
}
