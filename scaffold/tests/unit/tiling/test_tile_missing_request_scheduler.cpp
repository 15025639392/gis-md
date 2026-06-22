#include <gtest/gtest.h>

#include "earth_engine/content/GltfContentProvider.h"
#include "earth_engine/core/resources/FrameResourceBudget.h"
#include "earth_engine/providers/TerrainProvider.h"
#include "earth_engine/tiling/RasterMappedToTilesetTile.h"
#include "earth_engine/tiling/TileEmptyContentRegistry.h"
#include "earth_engine/tiling/TileMissingRequestScheduler.h"
#include "earth_engine/tiling/TileSelectionRootPolicy.h"
#include "earth_engine/tiling/TileTerminalLoadCommitter.h"
#include "earth_engine/tiling/TilesetTile.h"

#include <memory>
#include <string>
#include <unordered_map>

using namespace earth_engine;

namespace {

std::string cacheKeyForTile(const TileKey& key) {
    return key.schemeId + ":" +
           std::to_string(key.z) + ":" +
           std::to_string(key.x) + ":" +
           std::to_string(key.y);
}

class RetryContentProvider final : public TilesetContentProvider {
public:
    std::string id() const override { return "missing-retry-content"; }
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

    int requestCount = 0;
};

class TerrainQuadtreeContentProvider final : public TilesetContentProvider {
public:
    std::string id() const override { return "missing-terrain-content"; }
    bool supportsTile(const TileKey&) const override { return supports; }
    bool providesTerrainQuadtree() const override { return true; }
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

    bool supports = false;
    int requestCount = 0;
};

class RetryTerrainProvider final : public TerrainProvider {
public:
    std::string id() const override { return "missing-retry-terrain"; }
    std::string schemeId() const override { return scheme; }
    int minZoom() const override { return 0; }
    int maxZoom() const override { return 1; }
    int tileSize() const override { return 2; }
    bool supportsTile(const TileKey& key) const override {
        return key.schemeId == scheme;
    }
    std::string buildUrl(const TileKey&) const override {
        return "memory://missing-retry-terrain";
    }
    void requestTile(
        const TileKey& key,
        CancellationToken,
        TerrainCallback callback,
        HttpRequestPriority = HttpRequestPriority::Normal) override {
        ++requestCount;
        callback(key, TerrainTileLoadResult::retryLater());
    }
    std::unique_ptr<DecodedHeightmap> decodeTile(const uint8_t*, size_t)
        override {
        return nullptr;
    }

    int requestCount = 0;
    std::string scheme = "test";
};

} // namespace

TEST(TileMissingRequestSchedulerTest, RetriesAfterEmptyMarkerCleared) {
    TileLoadLifecycle lifecycle;
    FrameResourceBudgetConfig config;
    config.maxNetworkRequestsPerFrame = 4;
    config.maxNetworkInflight = 4;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);
    RetryContentProvider provider;
    TileEmptyContentRegistry emptyContentRegistry;
    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;
    std::unordered_map<std::string, std::unique_ptr<DecodedHeightmap>>
        terrainCache;

    const TileKey key{"test", 0, 0, 0};
    const std::string cacheKey = cacheKeyForTile(key);
    auto tile = std::make_unique<TilesetTile>(key, Rectangle{});
    tile->content.loadState = TileLoadState::FailedTemporarily;
    TilesetTile* tileRaw = tile.get();
    tiles[cacheKey] = std::move(tile);
    emptyContentRegistry.insert(cacheKey);

    auto requestOnce = [&]() {
        return TileMissingRequestScheduler::request(
            {TileLoadRequest{
                key,
                TileLoadPriorityGroup::Urgent,
                100.0}},
            TileMissingRequestSchedulerInput{
                lifecycle,
                budget,
                nullptr,
                &provider,
                tiles,
                terrainCache,
                emptyContentRegistry},
            cacheKeyForTile,
            [](TilesetTile&, double) { return false; },
            [&tiles](const TileKey& tileKey) -> TilesetTile* {
                const std::string lookupKey = cacheKeyForTile(tileKey);
                auto it = tiles.find(lookupKey);
                return it == tiles.end() ? nullptr : it->second.get();
            });
    };

    TileLoadRequestOutcome outcome = requestOnce();
    EXPECT_EQ(outcome.issued, 0u);
    EXPECT_EQ(provider.requestCount, 0);
    EXPECT_EQ(tileRaw->content.loadState, TileLoadState::FailedTemporarily);

    TileTerminalLoadCommitter::commitContentTerminalResult(
        *tileRaw,
        cacheKey,
        TileLoadResult::createTerminal(TileLoadStatus::RetryLater),
        emptyContentRegistry);
    outcome = requestOnce();
    EXPECT_EQ(outcome.issued, 1u);
    EXPECT_EQ(provider.requestCount, 1);
    EXPECT_EQ(lifecycle.counts().contentTerminalResults, 1u);
    EXPECT_EQ(tileRaw->content.loadState, TileLoadState::ContentLoading);
}

TEST(TileMissingRequestSchedulerTest, RetriesTerrainAfterEmptyMarkerCleared) {
    TileLoadLifecycle lifecycle;
    FrameResourceBudgetConfig config;
    config.maxNetworkRequestsPerFrame = 4;
    config.maxNetworkInflight = 4;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);
    RetryTerrainProvider provider;
    TileEmptyContentRegistry emptyContentRegistry;
    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;
    std::unordered_map<std::string, std::unique_ptr<DecodedHeightmap>>
        terrainCache;

    const TileKey key{"test", 0, 0, 0};
    const std::string cacheKey = cacheKeyForTile(key);
    auto tile = std::make_unique<TilesetTile>(key, Rectangle{});
    tile->content.loadState = TileLoadState::FailedTemporarily;
    TilesetTile* tileRaw = tile.get();
    tiles[cacheKey] = std::move(tile);
    emptyContentRegistry.insert(cacheKey);

    auto requestOnce = [&]() {
        return TileMissingRequestScheduler::request(
            {TileLoadRequest{
                key,
                TileLoadPriorityGroup::Urgent,
                100.0}},
            TileMissingRequestSchedulerInput{
                lifecycle,
                budget,
                &provider,
                nullptr,
                tiles,
                terrainCache,
                emptyContentRegistry},
            cacheKeyForTile,
            [](TilesetTile&, double) { return false; },
            [&tiles](const TileKey& tileKey) -> TilesetTile* {
                const std::string lookupKey = cacheKeyForTile(tileKey);
                auto it = tiles.find(lookupKey);
                return it == tiles.end() ? nullptr : it->second.get();
            });
    };

    TileLoadRequestOutcome outcome = requestOnce();
    EXPECT_EQ(outcome.issued, 0u);
    EXPECT_EQ(provider.requestCount, 0);
    EXPECT_EQ(tileRaw->content.loadState, TileLoadState::FailedTemporarily);

    TileTerminalLoadCommitter::commitTerrainTerminalResult(
        *tileRaw,
        cacheKey,
        TileLoadResult::createTerminal(TileLoadStatus::RetryLater),
        emptyContentRegistry);
    outcome = requestOnce();
    EXPECT_EQ(outcome.issued, 1u);
    EXPECT_EQ(provider.requestCount, 1);
    EXPECT_EQ(lifecycle.counts().terrainTerminalResults, 1u);
    EXPECT_EQ(tileRaw->content.loadState, TileLoadState::ContentLoading);
}

TEST(
    TileMissingRequestSchedulerTest,
    ContentOwnedTerrainQuadtreeDoesNotFallbackToTerrainProvider) {
    TileLoadLifecycle lifecycle;
    FrameResourceBudgetConfig config;
    config.maxNetworkRequestsPerFrame = 4;
    config.maxNetworkInflight = 4;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);
    TerrainQuadtreeContentProvider contentProvider;
    RetryTerrainProvider terrainProvider;
    TileEmptyContentRegistry emptyContentRegistry;
    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;
    std::unordered_map<std::string, std::unique_ptr<DecodedHeightmap>>
        terrainCache;

    const TileKey key{"test", 0, 0, 0};
    const TileLoadRequestOutcome outcome =
        TileMissingRequestScheduler::request(
            {TileLoadRequest{
                key,
                TileLoadPriorityGroup::Urgent,
                100.0}},
            TileMissingRequestSchedulerInput{
                lifecycle,
                budget,
                &terrainProvider,
                &contentProvider,
                tiles,
                terrainCache,
                emptyContentRegistry},
            cacheKeyForTile,
            [](TilesetTile&, double) { return false; },
            [&tiles](const TileKey& tileKey) -> TilesetTile* {
                const std::string lookupKey = cacheKeyForTile(tileKey);
                auto it = tiles.find(lookupKey);
                return it == tiles.end() ? nullptr : it->second.get();
            });

    EXPECT_EQ(outcome.issued, 0u);
    EXPECT_EQ(contentProvider.requestCount, 0);
    EXPECT_EQ(terrainProvider.requestCount, 0);
}

TEST(TileMissingRequestSchedulerTest, UpsampledTileUsesLocalPathBeforeContentProvider) {
    TileLoadLifecycle lifecycle;
    FrameResourceBudgetConfig config;
    config.maxNetworkRequestsPerFrame = 4;
    config.maxNetworkInflight = 4;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);
    RetryContentProvider provider;
    TileEmptyContentRegistry emptyContentRegistry;
    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;
    std::unordered_map<std::string, std::unique_ptr<DecodedHeightmap>>
        terrainCache;

    const TileKey key{"test", 1, 0, 0};
    const std::string cacheKey = cacheKeyForTile(key);
    auto tile = std::make_unique<TilesetTile>(key, Rectangle{});
    tile->content.upsampledFromParent = true;
    TilesetTile* tileRaw = tile.get();
    tiles[cacheKey] = std::move(tile);
    bool prepared = false;

    const TileLoadRequestOutcome outcome =
        TileMissingRequestScheduler::request(
            {TileLoadRequest{
                key,
                TileLoadPriorityGroup::Urgent,
                100.0}},
            TileMissingRequestSchedulerInput{
                lifecycle,
                budget,
                nullptr,
                &provider,
                tiles,
                terrainCache,
                emptyContentRegistry},
            cacheKeyForTile,
            [&prepared](TilesetTile&, double) {
                prepared = true;
                return true;
            },
            [&tiles](const TileKey& tileKey) -> TilesetTile* {
                const std::string lookupKey = cacheKeyForTile(tileKey);
                auto it = tiles.find(lookupKey);
                return it == tiles.end() ? nullptr : it->second.get();
            });

    EXPECT_EQ(outcome.issued, 1u);
    EXPECT_TRUE(prepared);
    EXPECT_EQ(provider.requestCount, 0);
    EXPECT_EQ(lifecycle.counts().terrainUploads, 1u);
    EXPECT_EQ(tileRaw->content.loadState, TileLoadState::ContentLoading);
}

TEST(TileMissingRequestSchedulerTest, VirtualTerrainRootNeverRequestsProvider) {
    TileLoadLifecycle lifecycle;
    FrameResourceBudgetConfig config;
    config.maxNetworkRequestsPerFrame = 4;
    config.maxNetworkInflight = 4;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);
    RetryTerrainProvider provider;
    provider.scheme = "Geographic-TMS";
    TileEmptyContentRegistry emptyContentRegistry;
    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;
    std::unordered_map<std::string, std::unique_ptr<DecodedHeightmap>>
        terrainCache;

    const TileKey key =
        TileSelectionRootPolicy::virtualTerrainRootKey("Geographic-TMS");
    const std::string cacheKey = cacheKeyForTile(key);
    auto tile = std::make_unique<TilesetTile>(key, Rectangle::MAXIMUM);
    tile->markEmptyContentDone();
    tile->unconditionallyRefine = true;
    tiles[cacheKey] = std::move(tile);

    const TileLoadRequestOutcome outcome =
        TileMissingRequestScheduler::request(
            {TileLoadRequest{
                key,
                TileLoadPriorityGroup::Urgent,
                100.0}},
            TileMissingRequestSchedulerInput{
                lifecycle,
                budget,
                &provider,
                nullptr,
                tiles,
                terrainCache,
                emptyContentRegistry},
            cacheKeyForTile,
            [](TilesetTile&, double) { return false; },
            [&tiles](const TileKey& tileKey) -> TilesetTile* {
                const std::string lookupKey = cacheKeyForTile(tileKey);
                auto it = tiles.find(lookupKey);
                return it == tiles.end() ? nullptr : it->second.get();
            });

    EXPECT_EQ(outcome.issued, 0u);
    EXPECT_EQ(provider.requestCount, 0);
    EXPECT_EQ(lifecycle.counts().terrainTerminalResults, 0u);
}
