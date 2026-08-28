#include <gtest/gtest.h>

#include "earth_engine/tiling/DirectRasterMapping.h"
#include "earth_engine/tiling/TileContentLifecycleManager.h"
#include "earth_engine/tiling/TilePendingUploadCompletion.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace earth_engine;

namespace {

class LifecycleTerrainContentProvider final : public TilesetContentProvider {
public:
    int requestCount = 0;

    std::string id() const override { return "content-terrain"; }
    bool supportsTile(const TileKey&) const override { return true; }
    bool providesTerrainQuadtree() const override { return true; }
    TileAvailabilityState availabilityState(
        const TileKey&) const override {
        return TileAvailabilityState::Available;
    }
    void requestTileContent(
        const TileKey& key,
        CancellationToken,
        ContentCallback callback,
        HttpRequestPriority = HttpRequestPriority::Normal) override {
        ++requestCount;
        callback(key, TileContentLoadResult::retryLater());
    }
    TileContentLoadResult decodeContent(const uint8_t*, size_t) override {
        return TileContentLoadResult::failed();
    }
};

class LifecycleOrdinaryContentProvider final : public TilesetContentProvider {
public:
    int requestCount = 0;

    std::string id() const override { return "ordinary-content"; }
    bool supportsTile(const TileKey&) const override { return true; }
    void requestTileContent(
        const TileKey& key,
        CancellationToken,
        ContentCallback callback,
        HttpRequestPriority = HttpRequestPriority::Normal) override {
        ++requestCount;
        callback(key, TileContentLoadResult::retryLater());
    }
    TileContentLoadResult decodeContent(const uint8_t*, size_t) override {
        return TileContentLoadResult::failed();
    }
};

class LifecycleLegacyTerrainProvider final : public TerrainProvider {
public:
    int requestCount = 0;

    std::string id() const override { return "legacy-terrain"; }
    std::string schemeId() const override { return "test"; }
    int minZoom() const override { return 0; }
    int maxZoom() const override { return 30; }
    int tileSize() const override { return 2; }
    std::string buildUrl(const TileKey&) const override { return ""; }
    void requestTile(
        const TileKey& key,
        CancellationToken,
        TerrainCallback callback,
        HttpRequestPriority = HttpRequestPriority::Normal) override {
        ++requestCount;
        callback(key, TerrainTileLoadResult::retryLater());
    }
    std::unique_ptr<DecodedHeightmap> decodeTile(
        const uint8_t*,
        size_t) override {
        return nullptr;
    }
};

} // namespace

TEST(TileContentLifecycleManagerTest, OwnsLifecycleState) {
    TileContentLifecycleManager manager;
    CancellationToken token;

    {
        std::lock_guard<std::mutex> lock(manager.loadLifecycle().mutex());
        ASSERT_TRUE(manager.loadLifecycle().requestState().beginTerrainRequest(
            "terrain",
            token));
    }

    EXPECT_EQ(1, manager.pendingRequests());
    EXPECT_TRUE(manager.hasPendingWork());

    std::atomic<bool> shutdownReturned{false};
    std::thread shutdownThread([&]() {
        manager.shutdown();
        shutdownReturned.store(true);
    });

    for (int i = 0; i < 200 && !token.isCancelled(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    EXPECT_TRUE(token.isCancelled());

    {
        std::lock_guard<std::mutex> lock(manager.loadLifecycle().mutex());
        manager.loadLifecycle()
            .requestState()
            .completeTerrainRequest("terrain");
    }
    manager.loadLifecycle().condition().notify_all();
    shutdownThread.join();

    EXPECT_TRUE(shutdownReturned.load());
    EXPECT_EQ(0, manager.pendingRequests());
    EXPECT_FALSE(manager.hasPendingWork());
}

TEST(TileContentLifecycleManagerTest, ExposesClaimedUploadWork) {
    TileContentLifecycleManager manager;
    FrameResourceBudgetConfig config;
    config.maxMainThreadFinalizesPerFrame = 1;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);

    {
        std::lock_guard<std::mutex> lock(manager.loadLifecycle().mutex());
        manager.loadLifecycle().pendingLoads().addUpload(PendingTileLoad{TileLoadDomain::Content,
                TileKey{"test", 0, 0, 0},
                "content-upload",
                TileLoadPriorityGroup::Normal,
                0.0,
                TileLoadResult::fromContentResult(
                    TileContentLoadResult::empty())});

        EXPECT_TRUE(manager.loadLifecycle()
                        .pendingLoads()
                        .takeHighestPriorityUpload(budget)
                        .has_value());
    }

    EXPECT_TRUE(manager.hasPendingWork());
    EXPECT_TRUE(manager.loadLifecycle().containsWorkForCacheKey(
        "content-upload"));

    TilePendingUploadCompletion::eraseUpload(
        manager.loadLifecycle(),
        "content-upload");

    EXPECT_FALSE(manager.hasPendingWork());
}

TEST(TileContentLifecycleManagerTest, ShutdownClearsClaimedUploadWork) {
    TileContentLifecycleManager manager;
    FrameResourceBudgetConfig config;
    config.maxMainThreadFinalizesPerFrame = 1;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);

    {
        std::lock_guard<std::mutex> lock(manager.loadLifecycle().mutex());
        manager.loadLifecycle().pendingLoads().addUpload(PendingTileLoad{TileLoadDomain::TerrainContent,
                TileKey{"test", 0, 0, 0},
                "terrain-upload",
                TileLoadPriorityGroup::Normal,
                0.0,
                TileLoadResult::createRenderableGltfTerrain(std::make_unique<GltfModel>())});

        EXPECT_TRUE(manager.loadLifecycle()
                        .pendingLoads()
                        .takeHighestPriorityUpload(budget)
                        .has_value());
    }

    EXPECT_TRUE(manager.hasPendingWork());
    EXPECT_TRUE(manager.loadLifecycle().containsWorkForCacheKey(
        "terrain-upload"));

    manager.shutdown();

    EXPECT_FALSE(manager.hasPendingWork());
    EXPECT_FALSE(manager.loadLifecycle().containsWorkForCacheKey(
        "terrain-upload"));
}
