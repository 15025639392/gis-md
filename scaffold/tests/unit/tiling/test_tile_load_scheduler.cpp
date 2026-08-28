#include <gtest/gtest.h>

#include "earth_engine/content/GltfContentProvider.h"
#include "earth_engine/core/resources/FrameResourceBudget.h"
#include "earth_engine/core/resources/SceneFrameResourceArbiter.h"
#include "earth_engine/providers/TerrainProvider.h"
#include "earth_engine/tiling/DirectRasterMapping.h"
#include "earth_engine/tiling/TileGltfTerrainUpsampledChildMaterializer.h"
#include "earth_engine/tiling/TileLoadLifecycle.h"
#include "earth_engine/tiling/TileLoadScheduler.h"
#include "earth_engine/tiling/TileUpsampleSourcePreparer.h"
#include "earth_engine/tiling/TilesetTile.h"

#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include <atomic>
#include <chrono>
#include <future>
#include <thread>

using namespace earth_engine;

namespace {

// clip worker 化后上采样结果由 AsyncSystem::pool worker 异步入队;自旋等
// pending 计数到位(worker 完成),超时返回 false。
template <typename Predicate>
bool waitForUpsampleAsync(Predicate&& predicate) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return predicate();
}

std::string cacheKeyForTile(const TileKey& key) {
    return key.schemeId.str() + ":" +
           std::to_string(key.z) + ":" +
           std::to_string(key.x) + ":" +
           std::to_string(key.y);
}

std::unique_ptr<GltfModel> makeSchedulerQuadTerrainGltfModel(
    const Rectangle& rectangle) {
    auto model = std::make_unique<GltfModel>();
    GltfPrimitive primitive;
    primitive.vertices.resize(4);
    primitive.vertices[0].positionEcef = Vec3(0.0, 0.0, 0.0);
    primitive.vertices[1].positionEcef = Vec3(1.0, 0.0, 0.0);
    primitive.vertices[2].positionEcef = Vec3(0.0, 1.0, 0.0);
    primitive.vertices[3].positionEcef = Vec3(1.0, 1.0, 0.0);
    for (SurfaceVertex& vertex : primitive.vertices) {
        vertex.normalEcef = Vec3::unitZ();
    }
    primitive.vertices[0].uv = {0.0f, 0.0f};
    primitive.vertices[1].uv = {1.0f, 0.0f};
    primitive.vertices[2].uv = {0.0f, 1.0f};
    primitive.vertices[3].uv = {1.0f, 1.0f};
    primitive.vertexTexCoords[0] = {
        std::array<float, 2>{0.0f, 0.0f},
        std::array<float, 2>{1.0f, 0.0f},
        std::array<float, 2>{0.0f, 1.0f},
        std::array<float, 2>{1.0f, 1.0f}};
    primitive.indices = {0, 1, 2, 1, 3, 2};
    model->primitives.push_back(std::move(primitive));
    model->rasterOverlayDetails.setGeographicRectangle(rectangle);
    return model;
}

class CountingContentProvider final : public TilesetContentProvider {
public:
    std::string id() const override { return "scheduler-content-inflight"; }
    bool supportsTile(const TileKey&) const override { return true; }
    int estimatedRequestFanout(const TileKey&) const override {
        return estimatedFanout;
    }
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
    int estimatedFanout = 1;
};

class TerrainQuadtreeContentProvider final : public TilesetContentProvider {
public:
    std::string id() const override {
        return "scheduler-content-terrain-quadtree";
    }
    bool supportsTile(const TileKey&) const override { return false; }
    bool providesTerrainQuadtree() const override { return true; }
    void requestTileContent(
        const TileKey&,
        CancellationToken,
        ContentCallback,
        HttpRequestPriority = HttpRequestPriority::Normal) override {
        ++requestCount;
    }
    void requestTileContent(
        const TileKey&,
        CancellationToken,
        ContentCallback,
        HttpRequestPriority,
        TileContentRequestOptions options) override {
        lastRequestOptions = std::move(options);
        ++requestCount;
    }
    TileContentLoadResult decodeContent(const uint8_t*, size_t) override {
        return TileContentLoadResult::failed();
    }

    int requestCount = 0;
    std::optional<TileContentRequestOptions> lastRequestOptions;
};

class ExplicitContentTerrainProvider final : public TerrainProvider,
                                             public TilesetContentProvider {
public:
    std::string id() const override { return "scheduler-content-terrain"; }
    std::string schemeId() const override { return "test"; }
    int minZoom() const override { return 0; }
    int maxZoom() const override { return 2; }
    int tileSize() const override { return 2; }
    bool supportsTile(const TileKey& key) const override {
        return TerrainProvider::supportsTile(key);
    }
    std::string buildUrl(const TileKey&) const override {
        return "memory://scheduler-content-terrain";
    }
    int estimatedRequestFanout(const TileKey&) const override {
        return estimatedFanout;
    }
    void requestTile(
        const TileKey&,
        CancellationToken,
        TerrainCallback,
        HttpRequestPriority = HttpRequestPriority::Normal) override {
        ++terrainRequestCount;
    }
    void requestTileContent(
        const TileKey&,
        CancellationToken,
        ContentCallback,
        HttpRequestPriority = HttpRequestPriority::Normal) override {
        ++contentRequestCount;
    }
    TileContentLoadResult decodeContent(const uint8_t*, size_t) override {
        return TileContentLoadResult::failed();
    }
    std::unique_ptr<DecodedHeightmap> decodeTile(const uint8_t*, size_t)
        override {
        return nullptr;
    }

    int terrainRequestCount = 0;
    int contentRequestCount = 0;
    int estimatedFanout = 1;
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
        TerrainCallback,
        HttpRequestPriority = HttpRequestPriority::Normal) override {
        ++requestCount;
    }
    std::unique_ptr<DecodedHeightmap> decodeTile(const uint8_t*, size_t)
        override {
        return nullptr;
    }

    int requestCount = 0;
};

class DeferredTerrainProvider final : public TerrainProvider {
public:
    std::string id() const override { return "scheduler-deferred-terrain"; }
    std::string schemeId() const override { return "test"; }
    int minZoom() const override { return 0; }
    int maxZoom() const override { return 2; }
    int tileSize() const override { return 2; }
    std::string buildUrl(const TileKey&) const override {
        return "memory://scheduler-deferred-terrain";
    }
    void requestTile(
        const TileKey&,
        CancellationToken,
        TerrainCallback,
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

TEST(TileLoadQueueTest, KeepsIndexedDeduplicationValidAcrossMutations) {
    const TileKey first{"test", 1, 0, 0};
    const TileKey second{"test", 1, 1, 0};
    const TileKey third{"test", 1, 2, 0};
    TileLoadQueue queue;
    queue.queue(
        first,
        TileLoadPriorityGroup::Preload,
        10.0);
    queue.queue(
        second,
        TileLoadPriorityGroup::Normal,
        20.0);
    queue.queue(
        third,
        TileLoadPriorityGroup::Normal,
        30.0);

    queue.erase(second);
    queue.queue(
        third,
        TileLoadPriorityGroup::Urgent,
        5.0);
    queue.queue(
        second,
        TileLoadPriorityGroup::Preload,
        40.0);

    ASSERT_EQ(queue.size(), 3u);
    EXPECT_EQ(queue.requests()[0].key, first);
    EXPECT_EQ(queue.requests()[1].key, third);
    EXPECT_EQ(
        queue.requests()[1].group,
        TileLoadPriorityGroup::Urgent);
    EXPECT_EQ(queue.requests()[2].key, second);

    queue.eraseIf([&](const TileLoadRequest& request) {
        return request.key == first;
    });
    queue.queue(
        second,
        TileLoadPriorityGroup::Normal,
        1.0);
    ASSERT_EQ(queue.size(), 2u);
    EXPECT_EQ(queue.requests()[0].key, third);
    EXPECT_EQ(queue.requests()[1].key, second);
    EXPECT_EQ(
        queue.requests()[1].group,
        TileLoadPriorityGroup::Normal);

    std::vector<TileLoadRequest> taken = queue.takeRequests();
    EXPECT_TRUE(queue.empty());
    ASSERT_EQ(taken.size(), 2u);

    queue.mergeRequests(std::move(taken));
    queue.mergeRequests(
        {TileLoadRequest{
            third,
            TileLoadPriorityGroup::Preload,
            0.0}});
    ASSERT_EQ(queue.size(), 2u);
    EXPECT_EQ(
        queue.front().group,
        TileLoadPriorityGroup::Urgent);

    queue.resize(1);
    queue.queue(
        second,
        TileLoadPriorityGroup::Urgent,
        0.5);
    ASSERT_EQ(queue.size(), 2u);
    EXPECT_EQ(queue.requests()[1].key, second);
}

TEST(TileLoadSchedulerTest,
     ConsumableQueueDropsIssuedAndTerminalRequests) {
    TileLoadLifecycle lifecycle;
    FrameResourceBudgetConfig config;
    config.maxNetworkRequestsPerFrame = 4;
    config.maxNetworkInflight = 4;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);

    const TileKey doneKey{"test", 1, 0, 0};
    const TileKey loadKey{"test", 1, 1, 0};
    TilesetTile doneTile(doneKey, Rectangle{});
    doneTile.content.loadState = TileLoadState::Done;
    CountingContentProvider provider;
    TileLoadQueue queue;
    queue.queue(doneKey, TileLoadPriorityGroup::Normal, 2.0);
    queue.queue(loadKey, TileLoadPriorityGroup::Urgent, 1.0);

    const TileLoadRequestOutcome outcome =
        TileLoadScheduler::requestMissingTiles(
            queue,
            TileLoadSchedulerInput{lifecycle, budget, &provider},
            cacheKeyForTile,
            [&](const TileKey& key,
                const std::string&,
                TilesetTile*& tileState) {
                TileLoadRequestSnapshot snapshot;
                if (key == doneKey) {
                    tileState = &doneTile;
                    snapshot.hasTile = true;
                    snapshot.loadState = TileLoadState::Done;
                } else {
                    tileState = nullptr;
                    snapshot.contentProviderSupportsTile = true;
                }
                return snapshot;
            },
            [](const std::string&) { return false; },
            [](TilesetTile&, double) { return false; },
            [](const TileKey&) {});

    EXPECT_EQ(outcome.issued, 1u);
    EXPECT_EQ(outcome.classifiedContent, 1u);
    EXPECT_EQ(outcome.issuedContent, 1u);
    EXPECT_EQ(outcome.classifiedTerrainAvailabilityUpsample, 0u);
    EXPECT_EQ(outcome.classifiedRasterDetailUpsample, 0u);
    EXPECT_EQ(outcome.skippedClassified, 1u);
    EXPECT_TRUE(queue.empty());
    EXPECT_EQ(provider.requestCount, 1);
}

TEST(TileLoadSchedulerTest,
     TerrainContentRequestCarriesStableRequiredProjectionSnapshot) {
    TileLoadLifecycle lifecycle;
    FrameResourceBudgetConfig config;
    config.maxNetworkRequestsPerFrame = 4;
    config.maxNetworkInflight = 4;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);

    const TileKey key{"Geographic-TMS", 2, 1, 1};
    TilesetTile tile(key, Rectangle{});
    TerrainQuadtreeContentProvider provider;
    const std::vector<RasterOverlayProjection> requiredProjections{
        RasterOverlayProjection::WebMercator,
        RasterOverlayProjection::Geographic};

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
                &requiredProjections},
            cacheKeyForTile,
            [&tile](
                const TileKey&,
                const std::string&,
                TilesetTile*& tileState) {
                tileState = &tile;
                TileLoadRequestSnapshot snapshot;
                snapshot.hasTile = true;
                snapshot.contentProviderSupportsTile = true;
                snapshot.contentProviderOwnsTerrainQuadtree = true;
                return snapshot;
            },
            [](const std::string&) { return false; },
            [](TilesetTile&, double) { return false; },
            [](const TileKey&) {});

    EXPECT_EQ(1u, outcome.issued);
    ASSERT_TRUE(provider.lastRequestOptions.has_value());
    EXPECT_FALSE(
        provider.lastRequestOptions
            ->generateTerrainRasterOverlayDetails);
    EXPECT_EQ(
        requiredProjections,
        provider.lastRequestOptions
            ->requiredRasterOverlayProjections);
}

TEST(TileLoadSchedulerTest,
     ConsumableQueueRetainsRetryableWorkAndMergesNewSourceRequests) {
    TileLoadLifecycle lifecycle;
    FrameResourceBudgetConfig config;
    config.maxNetworkRequestsPerFrame = 4;
    config.maxNetworkInflight = 4;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);

    const TileKey retryKey{"test", 2, 0, 0};
    const TileKey childKey{"test", 2, 1, 0};
    const TileKey parentKey{"test", 1, 0, 0};
    TilesetTile retryTile(retryKey, Rectangle{});
    retryTile.content.loadState = TileLoadState::FailedTemporarily;
    retryTile.temporaryFailureRetryNotBeforeMs =
        std::numeric_limits<double>::max();
    TilesetTile child(childKey, Rectangle{});
    child.content.markTerrainAvailabilityUpsample();
    TileLoadQueue queue;
    queue.queue(retryKey, TileLoadPriorityGroup::Normal, 2.0);
    queue.queue(childKey, TileLoadPriorityGroup::Urgent, 1.0);

    const TileLoadRequestOutcome outcome =
        TileLoadScheduler::requestMissingTiles(
            queue,
            TileLoadSchedulerInput{lifecycle, budget, nullptr},
            cacheKeyForTile,
            [&](const TileKey& key,
                const std::string&,
                TilesetTile*& tileState) {
                TileLoadRequestSnapshot snapshot;
                snapshot.hasTile = true;
                if (key == retryKey) {
                    tileState = &retryTile;
                    snapshot.loadState = TileLoadState::FailedTemporarily;
                } else {
                    tileState = &child;
                    snapshot.upsampleKind =
                        TileContentUpsampleKind::TerrainAvailability;
                }
                return snapshot;
            },
            [](const std::string&) { return false; },
            [&](TilesetTile&, double priority) {
                queue.queue(
                    parentKey,
                    TileLoadPriorityGroup::Urgent,
                    priority);
                return false;
            },
            [](const TileKey&) {});

    EXPECT_EQ(outcome.issued, 0u);
    EXPECT_EQ(outcome.skippedClassified, 1u);
    EXPECT_EQ(outcome.skippedUpsampleSourceNotReady, 1u);
    EXPECT_EQ(queue.size(), 3u);
    const auto contains = [&](const TileKey& key) {
        return std::any_of(
            queue.begin(),
            queue.end(),
            [&](const TileLoadRequest& request) {
                return request.key == key;
            });
    };
    EXPECT_TRUE(contains(retryKey));
    EXPECT_TRUE(contains(childKey));
    EXPECT_TRUE(contains(parentKey));
}

TEST(TileLoadSchedulerTest,
     ConsumableQueueRetainsBlockedRequestAndLowerPriorities) {
    TileLoadLifecycle lifecycle;
    FrameResourceBudgetConfig config;
    config.maxNetworkRequestsPerFrame = 4;
    config.maxNetworkInflight = 1;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);
    CancellationToken token;
    {
        std::lock_guard<std::mutex> lock(lifecycle.mutex());
        ASSERT_TRUE(lifecycle.requestState().beginContentRequest(
            "busy",
            token));
    }

    const TileKey urgentKey{"test", 1, 0, 0};
    const TileKey normalKey{"test", 1, 1, 0};
    CountingContentProvider provider;
    TileLoadQueue queue;
    queue.queue(normalKey, TileLoadPriorityGroup::Normal, 1.0);
    queue.queue(urgentKey, TileLoadPriorityGroup::Urgent, 2.0);

    const TileLoadRequestOutcome outcome =
        TileLoadScheduler::requestMissingTiles(
            queue,
            TileLoadSchedulerInput{lifecycle, budget, &provider},
            cacheKeyForTile,
            [](const TileKey&,
               const std::string&,
               TilesetTile*& tileState) {
                tileState = nullptr;
                TileLoadRequestSnapshot snapshot;
                snapshot.contentProviderSupportsTile = true;
                return snapshot;
            },
            [](const std::string&) { return false; },
            [](TilesetTile&, double) { return false; },
            [](const TileKey&) {});

    EXPECT_EQ(outcome.issued, 0u);
    EXPECT_TRUE(outcome.blockedByInflight);
    ASSERT_EQ(queue.size(), 2u);
    EXPECT_EQ(queue.front().key, urgentKey);
    EXPECT_EQ(provider.requestCount, 0);

    {
        std::lock_guard<std::mutex> lock(lifecycle.mutex());
        lifecycle.requestState().completeContentRequest("busy");
    }
}

TEST(TileLoadSchedulerTest,
     ConsumedBatchEntryRemovesDependencyDuplicateQueuedDuringThePass) {
    TileLoadLifecycle lifecycle;
    FrameResourceBudgetConfig config;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);

    const TileKey childKey{"test", 2, 0, 0};
    const TileKey parentKey{"test", 1, 0, 0};
    TilesetTile child(childKey, Rectangle{});
    child.content.markTerrainAvailabilityUpsample();
    TilesetTile parent(parentKey, Rectangle{});
    parent.content.loadState = TileLoadState::Done;
    TileLoadQueue queue;
    queue.queue(childKey, TileLoadPriorityGroup::Urgent, 1.0);
    queue.queue(parentKey, TileLoadPriorityGroup::Normal, 2.0);

    const TileLoadRequestOutcome outcome =
        TileLoadScheduler::requestMissingTiles(
            queue,
            TileLoadSchedulerInput{lifecycle, budget, nullptr},
            cacheKeyForTile,
            [&](const TileKey& key,
                const std::string&,
                TilesetTile*& tileState) {
                TileLoadRequestSnapshot snapshot;
                snapshot.hasTile = true;
                if (key == childKey) {
                    tileState = &child;
                    snapshot.upsampleKind =
                        TileContentUpsampleKind::TerrainAvailability;
                } else {
                    tileState = &parent;
                    snapshot.loadState = TileLoadState::Done;
                }
                return snapshot;
            },
            [](const std::string&) { return false; },
            [&](TilesetTile&, double priority) {
                queue.queue(
                    parentKey,
                    TileLoadPriorityGroup::Urgent,
                    priority);
                return false;
            },
            [](const TileKey&) {});

    EXPECT_EQ(outcome.skippedUpsampleSourceNotReady, 1u);
    EXPECT_EQ(outcome.skippedClassified, 1u);
    ASSERT_EQ(queue.size(), 1u);
    EXPECT_EQ(queue.front().key, childKey);
}

TEST(
    TileLoadSchedulerTest,
    LocalUpsampleInflightDoesNotBlockNetworkContent) {
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

    EXPECT_EQ(outcome.issued, 1u);
    EXPECT_FALSE(outcome.blockedByInflight);
    EXPECT_TRUE(planned);
    EXPECT_TRUE(marked);
    EXPECT_EQ(provider.requestCount, 1);

    {
        std::lock_guard<std::mutex> lock(lifecycle.mutex());
        lifecycle.requestState().completeTerrainRequest("busy");
    }
}

TEST(
    TileLoadSchedulerTest,
    UpsampleClipCapacityDoesNotBlockFollowingNetworkContent) {
    TileLoadLifecycle lifecycle;
    TileLoadLifecycle secondLifecycle;
    FrameResourceBudgetConfig config;
    config.maxNetworkRequestsPerFrame = 20;
    config.maxNetworkInflight = 20;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);

    const size_t upsampleCapacity =
        TileLoadRequestDispatcher::maximumUpsampleClipInflight();
    std::promise<void> releasePromise;
    const std::shared_future<void> release =
        releasePromise.get_future().share();
    for (size_t i = 0; i < upsampleCapacity; ++i) {
        TileLoadLifecycle& owner =
            i % 2u == 0u ? lifecycle : secondLifecycle;
        const TileKey key{
            "busy",
            1,
            static_cast<int32_t>(i),
            0};
        const TileLoadDispatchResult result =
            TileLoadRequestDispatcher::requestUpsampleClip(
                owner.mutex(),
                owner.condition(),
                owner.requestState(),
                owner.pendingLoads(),
                key,
                "busy-upsample-" + std::to_string(i),
                TileLoadPriorityGroup::Normal,
                0.0,
                0,
                [release](const int&) {
                    release.wait();
                    return TileLoadResult::createTerminal(
                        TileLoadStatus::Failed);
                },
                [] {});
        EXPECT_EQ(result, TileLoadDispatchResult::Issued);
    }
    EXPECT_FALSE(
        TileLoadRequestDispatcher::hasUpsampleClipWorkerCapacity());

    const TileKey parentKey{"test", 0, 0, 0};
    const TileKey childKey{"test", 1, 0, 0};
    const TileKey contentKey{"test", 1, 1, 0};
    const Rectangle parentBounds{-1.0, -0.5, 1.0, 0.5};
    const Rectangle childBounds{-1.0, -0.5, 0.0, 0.0};
    TilesetTile parent(parentKey, parentBounds);
    TilesetTile child(childKey, childBounds, &parent);
    child.content.markRasterDetailUpsample(
        RasterOverlayProjection::Geographic);
    parent.content.renderContent.setGltfContent(
        makeSchedulerQuadTerrainGltfModel(parentBounds));
    parent.content.renderContent.setTerrainRenderContent(true);
    parent.markRenderContentDone();
    bool prepared = false;
    bool contentMarked = false;
    CountingContentProvider provider;

    TileLoadQueue queue;
    queue.queue(
        childKey,
        TileLoadPriorityGroup::Normal,
        1.0);
    queue.queue(
        contentKey,
        TileLoadPriorityGroup::Normal,
        2.0);
    const TileLoadRequestOutcome outcome =
        TileLoadScheduler::requestMissingTiles(
            queue,
            TileLoadSchedulerInput{lifecycle, budget, &provider},
            cacheKeyForTile,
            [&](const TileKey& key,
                const std::string&,
                TilesetTile*& tileState) {
                TileLoadRequestSnapshot snapshot;
                if (key == contentKey) {
                    tileState = nullptr;
                    snapshot.contentProviderSupportsTile = true;
                    return snapshot;
                }
                tileState = &child;
                snapshot.hasTile = true;
                snapshot.upsampleKind =
                    TileContentUpsampleKind::RasterDetail;
                snapshot.contentProviderOwnsTerrainQuadtree = true;
                return snapshot;
            },
            [](const std::string&) { return false; },
            [&](TilesetTile&, double) {
                prepared = true;
                return true;
            },
            [&](const TileKey& key) {
                if (key == contentKey) {
                    contentMarked = true;
                }
            });

    EXPECT_TRUE(prepared);
    EXPECT_TRUE(contentMarked);
    EXPECT_EQ(provider.requestCount, 1);
    EXPECT_EQ(outcome.issued, 1u);
    EXPECT_EQ(outcome.issuedContent, 1u);
    EXPECT_EQ(outcome.skippedUpsampleWorkerCapacity, 1u);
    EXPECT_EQ(queue.size(), 1u);
    EXPECT_EQ(queue.front().key, childKey);
    EXPECT_EQ(
        lifecycle.pendingRequestCount() +
            secondLifecycle.pendingRequestCount(),
        upsampleCapacity);
    EXPECT_EQ(budget.networkRequestsIssued(), 1u);

    releasePromise.set_value();
    EXPECT_TRUE(waitForUpsampleAsync([&] {
        return lifecycle.pendingRequestCount() == 0u &&
               secondLifecycle.pendingRequestCount() == 0u;
    }));
    EXPECT_TRUE(
        TileLoadRequestDispatcher::hasUpsampleClipWorkerCapacity());
}

TEST(TileLoadSchedulerTest, UpsampleClipConsumesSceneWorkerDispatchGrant) {
    TileLoadLifecycle lifecycle;
    const TileKey key{"worker-budget", 1, 0, 0};

    SceneFrameResourceArbiter arbiter;
    SceneFrameResourceArbiterConfig deniedConfig;
    deniedConfig.workerDispatch.maxUnitsPerFrame = 0;
    arbiter.beginFrame(1, deniedConfig);
    ASSERT_TRUE(arbiter.declareDemand(
        SceneFrameResourceProducer::Terrain,
        SceneFrameResourceStage::WorkerDispatch,
        FrameResourcePriority::Normal));
    ASSERT_TRUE(arbiter.sealAllocations());

    bool issued = false;
    EXPECT_EQ(
        TileLoadDispatchResult::Blocked,
        TileLoadRequestDispatcher::requestUpsampleClip(
            lifecycle.mutex(),
            lifecycle.condition(),
            lifecycle.requestState(),
            lifecycle.pendingLoads(),
            key,
            "worker-budget-denied",
            TileLoadPriorityGroup::Normal,
            0.0,
            0,
            [](const int&) {
                return TileLoadResult::createTerminal(TileLoadStatus::Failed);
            },
            [&issued]() { issued = true; },
            &arbiter));
    EXPECT_FALSE(issued);
    EXPECT_EQ(0u, lifecycle.pendingRequestCount());
    EXPECT_EQ(0u, arbiter.used(
                      SceneFrameResourceProducer::Terrain,
                      SceneFrameResourceStage::WorkerDispatch,
                      FrameResourcePriority::Normal));

    SceneFrameResourceArbiterConfig allowedConfig;
    allowedConfig.workerDispatch.maxUnitsPerFrame = 1;
    arbiter.beginFrame(2, allowedConfig);
    ASSERT_TRUE(arbiter.declareDemand(
        SceneFrameResourceProducer::Terrain,
        SceneFrameResourceStage::WorkerDispatch,
        FrameResourcePriority::Normal));
    ASSERT_TRUE(arbiter.sealAllocations());

    EXPECT_EQ(
        TileLoadDispatchResult::Issued,
        TileLoadRequestDispatcher::requestUpsampleClip(
            lifecycle.mutex(),
            lifecycle.condition(),
            lifecycle.requestState(),
            lifecycle.pendingLoads(),
            key,
            "worker-budget-allowed",
            TileLoadPriorityGroup::Normal,
            0.0,
            0,
            [](const int&) {
                return TileLoadResult::createTerminal(TileLoadStatus::Failed);
            },
            [&issued]() { issued = true; },
            &arbiter));
    EXPECT_TRUE(issued);
    EXPECT_EQ(1u, arbiter.used(
                      SceneFrameResourceProducer::Terrain,
                      SceneFrameResourceStage::WorkerDispatch,
                      FrameResourcePriority::Normal));
    EXPECT_TRUE(waitForUpsampleAsync(
        [&]() { return lifecycle.pendingRequestCount() == 0u; }));
}

TEST(
    TileLoadSchedulerTest,
    NetworkInflightBlockDoesNotBlockFollowingLocalUpsample) {
    TileLoadLifecycle lifecycle;
    FrameResourceBudgetConfig config;
    config.maxNetworkRequestsPerFrame = 20;
    config.maxNetworkInflight = 1;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);
    CancellationToken token;
    {
        std::lock_guard<std::mutex> lock(lifecycle.mutex());
        ASSERT_TRUE(lifecycle.requestState().beginContentRequest(
            "busy-network",
            token));
    }

    const TileKey parentKey{"test", 0, 0, 0};
    const TileKey childKey{"test", 1, 0, 0};
    const TileKey contentKey{"test", 1, 1, 0};
    const Rectangle parentBounds{-1.0, -0.5, 1.0, 0.5};
    const Rectangle childBounds{-1.0, -0.5, 0.0, 0.0};
    TilesetTile parent(parentKey, parentBounds);
    TilesetTile child(childKey, childBounds, &parent);
    child.content.markRasterDetailUpsample(
        RasterOverlayProjection::Geographic);
    parent.content.renderContent.setGltfContent(
        makeSchedulerQuadTerrainGltfModel(parentBounds));
    parent.content.renderContent.setTerrainRenderContent(true);
    parent.markRenderContentDone();
    CountingContentProvider provider;
    TileLoadQueue queue;
    queue.queue(
        contentKey,
        TileLoadPriorityGroup::Urgent,
        1.0);
    queue.queue(
        childKey,
        TileLoadPriorityGroup::Normal,
        2.0);

    const TileLoadRequestOutcome outcome =
        TileLoadScheduler::requestMissingTiles(
            queue,
            TileLoadSchedulerInput{lifecycle, budget, &provider},
            cacheKeyForTile,
            [&](const TileKey& key,
                const std::string&,
                TilesetTile*& tileState) {
                TileLoadRequestSnapshot snapshot;
                if (key == contentKey) {
                    tileState = nullptr;
                    snapshot.contentProviderSupportsTile = true;
                    return snapshot;
                }
                tileState = &child;
                snapshot.hasTile = true;
                snapshot.upsampleKind =
                    TileContentUpsampleKind::RasterDetail;
                snapshot.contentProviderOwnsTerrainQuadtree = true;
                return snapshot;
            },
            [](const std::string&) { return false; },
            [](TilesetTile&, double) { return true; },
            [](const TileKey&) {});

    EXPECT_TRUE(outcome.blockedByInflight);
    EXPECT_EQ(outcome.issued, 1u);
    EXPECT_EQ(outcome.issuedRasterDetailUpsample, 1u);
    EXPECT_EQ(provider.requestCount, 0);
    ASSERT_EQ(queue.size(), 1u);
    EXPECT_EQ(queue.front().key, contentKey);
    EXPECT_TRUE(waitForUpsampleAsync(
        [&] { return lifecycle.pendingRequestCount() == 1u; }));

    {
        std::lock_guard<std::mutex> lock(lifecycle.mutex());
        lifecycle.requestState().completeContentRequest(
            "busy-network");
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

TEST(TileLoadSchedulerTest, IgnoresLegacyTerrainFanoutInRuntimeScheduler) {
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
                nullptr},
            cacheKeyForTile,
            [](const TileKey&,
               const std::string&,
               TilesetTile*& tileState) {
                tileState = nullptr;
                TileLoadRequestSnapshot snapshot;
                return snapshot;
            },
            [](const std::string&) { return false; },
            [](TilesetTile&, double) { return false; },
            [&marked](const TileKey&) { marked = true; });

    EXPECT_EQ(outcome.issued, 0u);
    EXPECT_FALSE(outcome.blockedByInflight);
    EXPECT_EQ(provider.requestCount, 0);
    EXPECT_FALSE(marked);
    EXPECT_EQ(lifecycle.pendingRequestCount(), 1u);

    {
        std::lock_guard<std::mutex> lock(lifecycle.mutex());
        lifecycle.requestState().completeTerrainRequest("busy");
    }
}

TEST(TileLoadSchedulerTest, ExplicitContentProviderUsesContentRequestPath) {
    TileLoadLifecycle lifecycle;
    FrameResourceBudgetConfig config;
    config.maxNetworkInflight = 4;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);

    const TileKey key{"test", 0, 0, 0};
    ExplicitContentTerrainProvider provider;
    bool marked = false;

    const TileLoadRequestOutcome outcome =
        TileLoadScheduler::requestMissingTiles(
            {TileLoadRequest{
                key,
                TileLoadPriorityGroup::Normal,
                0.0}},
            TileLoadSchedulerInput{
                lifecycle,
                budget,
                &provider},
            cacheKeyForTile,
            [](const TileKey&,
               const std::string&,
               TilesetTile*& tileState) {
                tileState = nullptr;
                TileLoadRequestSnapshot snapshot;
                snapshot.contentProviderSupportsTile = true;
                return snapshot;
            },
            [](const std::string&) { return false; },
            [](TilesetTile&, double) { return false; },
            [&marked](const TileKey&) { marked = true; });

    EXPECT_EQ(outcome.issued, 1u);
    EXPECT_EQ(outcome.classifiedContent, 1u);
    EXPECT_EQ(outcome.issuedContent, 1u);
    EXPECT_EQ(outcome.classifiedTerrainAvailabilityUpsample, 0u);
    EXPECT_EQ(outcome.classifiedRasterDetailUpsample, 0u);
    EXPECT_FALSE(outcome.blockedByInflight);
    EXPECT_EQ(provider.contentRequestCount, 1);
    EXPECT_EQ(provider.terrainRequestCount, 0);
    EXPECT_TRUE(marked);
}

TEST(TileLoadSchedulerTest, BlocksContentFanoutOverInflightCapacity) {
    TileLoadLifecycle lifecycle;
    FrameResourceBudgetConfig config;
    config.maxNetworkInflight = 2;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);
    CancellationToken token;
    {
        std::lock_guard<std::mutex> lock(lifecycle.mutex());
        ASSERT_TRUE(lifecycle.requestState().beginContentRequest(
            "busy",
            token));
    }

    const TileKey key{"test", 0, 0, 0};
    CountingContentProvider provider;
    provider.estimatedFanout = 2;
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
                &provider},
            cacheKeyForTile,
            [](const TileKey&,
               const std::string&,
               TilesetTile*& tileState) {
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
    EXPECT_EQ(provider.requestCount, 0);
    EXPECT_FALSE(marked);
    EXPECT_EQ(lifecycle.pendingRequestCount(), 1u);

    {
        std::lock_guard<std::mutex> lock(lifecycle.mutex());
        lifecycle.requestState().completeContentRequest("busy");
    }
}

TEST(TileLoadSchedulerTest, PendingUploadsRemainIndependentWhenLegacyTerrainIsIgnored) {
    TileLoadLifecycle lifecycle;
    FrameResourceBudgetConfig config;
    config.maxNetworkInflight = 2;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);

    const TileKey firstUploadKey{"test", 1, 0, 0};
    const TileKey secondUploadKey{"test", 1, 1, 0};
    const TileKey requestKey{"test", 1, 2, 0};
    {
        std::lock_guard<std::mutex> lock(lifecycle.mutex());
        lifecycle.pendingLoads().addUpload(PendingTileLoad{TileLoadDomain::TerrainContent,
            firstUploadKey,
            cacheKeyForTile(firstUploadKey),
            TileLoadPriorityGroup::Normal,
            0.0,
            TileLoadResult::createRenderableGltfTerrain(std::make_unique<GltfModel>())});
        lifecycle.pendingLoads().addUpload(PendingTileLoad{TileLoadDomain::TerrainContent,
            secondUploadKey,
            cacheKeyForTile(secondUploadKey),
            TileLoadPriorityGroup::Normal,
            0.0,
            TileLoadResult::createRenderableGltfTerrain(std::make_unique<GltfModel>())});
    }

    DeferredTerrainProvider provider;
    bool marked = false;

    const TileLoadRequestOutcome outcome =
        TileLoadScheduler::requestMissingTiles(
            {TileLoadRequest{
                requestKey,
                TileLoadPriorityGroup::Urgent,
                100.0}},
            TileLoadSchedulerInput{
                lifecycle,
                budget,
                nullptr},
            cacheKeyForTile,
            [](const TileKey&,
               const std::string&,
               TilesetTile*& tileState) {
                tileState = nullptr;
                TileLoadRequestSnapshot snapshot;
                return snapshot;
            },
            [](const std::string&) { return false; },
            [](TilesetTile&, double) { return false; },
            [&marked](const TileKey&) { marked = true; });

    EXPECT_EQ(outcome.issued, 0u);
    EXPECT_FALSE(outcome.blockedByInflight);
    EXPECT_EQ(provider.requestCount, 0);
    EXPECT_FALSE(marked);
    EXPECT_EQ(lifecycle.counts().gltfTerrainUploads, 2u);
    EXPECT_EQ(lifecycle.pendingRequestCount(), 0u);
}

TEST(TileLoadSchedulerTest,
     TerrainContentUpsampleWithoutGltfSourceDoesNotQueueLegacyTerrainUpload) {
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
    tile.content.markTerrainAvailabilityUpsample();
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
                nullptr},
            cacheKeyForTile,
            [&tile](
                const TileKey&,
                const std::string&,
                TilesetTile*& tileState) {
                tileState = &tile;
                TileLoadRequestSnapshot snapshot;
                snapshot.hasTile = true;
                snapshot.upsampleKind = TileContentUpsampleKind::TerrainAvailability;
                return snapshot;
            },
            [](const std::string&) { return false; },
            [&prepared](TilesetTile&, double) {
                prepared = true;
                return true;
            },
            [&marked](const TileKey&) { marked = true; });

    EXPECT_EQ(outcome.issued, 0u);
    EXPECT_FALSE(outcome.blockedByInflight);
    EXPECT_TRUE(prepared);
    EXPECT_FALSE(marked);
    EXPECT_EQ(lifecycle.counts().gltfTerrainUploads, 0u);
    EXPECT_EQ(lifecycle.pendingRequestCount(), 1u);

    {
        std::lock_guard<std::mutex> lock(lifecycle.mutex());
        lifecycle.requestState().completeTerrainRequest("busy");
        lifecycle.pendingLoads().clear();
    }
}

TEST(TileLoadSchedulerTest,
     ContentTerrainQuadtreeUpsampleWithoutGltfSourceNeverQueuesLegacyDomain) {
    TileLoadLifecycle lifecycle;
    FrameResourceBudgetConfig config;
    config.maxNetworkRequestsPerFrame = 4;
    config.maxNetworkInflight = 4;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);

    const TileKey parentKey{"test", 0, 0, 0};
    const TileKey childKey{"test", 1, 0, 0};
    TilesetTile parent(parentKey, Rectangle{});
    TilesetTile child(childKey, Rectangle{}, &parent);
    child.content.markTerrainAvailabilityUpsample();
    TerrainQuadtreeContentProvider provider;
    FanoutTerrainProvider legacyProvider;
    bool prepared = false;
    bool marked = false;

    const TileLoadRequestOutcome outcome =
        TileLoadScheduler::requestMissingTiles(
            {TileLoadRequest{
                childKey,
                TileLoadPriorityGroup::Urgent,
                100.0}},
            TileLoadSchedulerInput{
                lifecycle,
                budget,
                &provider},
            cacheKeyForTile,
            [&child](
                const TileKey&,
                const std::string&,
                TilesetTile*& tileState) {
                tileState = &child;
                TileLoadRequestSnapshot snapshot;
                snapshot.hasTile = true;
                snapshot.upsampleKind =
                    TileContentUpsampleKind::TerrainAvailability;
                snapshot.contentProviderOwnsTerrainQuadtree = true;
                return snapshot;
            },
            [](const std::string&) { return false; },
            [&prepared](TilesetTile&, double) {
                prepared = true;
                return true;
            },
            [&marked](const TileKey&) { marked = true; });

    EXPECT_EQ(outcome.issued, 0u);
    EXPECT_FALSE(outcome.blockedByInflight);
    EXPECT_TRUE(prepared);
    EXPECT_FALSE(marked);
    EXPECT_EQ(lifecycle.counts().gltfTerrainUploads, 0u);
    EXPECT_EQ(lifecycle.counts().contentUploads, 0u);
    EXPECT_EQ(lifecycle.pendingRequestCount(), 0u);
    EXPECT_EQ(provider.requestCount, 0);
    EXPECT_EQ(legacyProvider.requestCount, 0);
}

TEST(TileLoadSchedulerTest,
     ContentTerrainUpsampleQueuesGltfLoadResultBeforeCommit) {
    TileLoadLifecycle lifecycle;
    FrameResourceBudgetConfig config;
    config.maxNetworkRequestsPerFrame = 4;
    config.maxNetworkInflight = 4;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);

    const TileKey parentKey{"test", 0, 0, 0};
    const TileKey childKey{"test", 1, 0, 0};
    const Rectangle parentBounds{-1.0, -0.5, 1.0, 0.5};
    const Rectangle childBounds{-1.0, -0.5, 0.0, 0.0};
    TilesetTile parent(parentKey, parentBounds);
    TilesetTile child(childKey, childBounds, &parent);
    child.content.markTerrainAvailabilityUpsample();
    parent.content.renderContent.setGltfContent(
        makeSchedulerQuadTerrainGltfModel(parentBounds));
    parent.content.renderContent.setTerrainRenderContent(true);
    parent.markRenderContentDone();

    TerrainQuadtreeContentProvider provider;
    FanoutTerrainProvider legacyProvider;
    bool prepared = false;
    bool marked = false;

    const TileLoadRequestOutcome outcome =
        TileLoadScheduler::requestMissingTiles(
            {TileLoadRequest{
                childKey,
                TileLoadPriorityGroup::Urgent,
                100.0}},
            TileLoadSchedulerInput{
                lifecycle,
                budget,
                &provider},
            cacheKeyForTile,
            [&child](
                const TileKey&,
                const std::string&,
                TilesetTile*& tileState) {
                tileState = &child;
                TileLoadRequestSnapshot snapshot;
                snapshot.hasTile = true;
                snapshot.upsampleKind = TileContentUpsampleKind::TerrainAvailability;
                snapshot.contentProviderOwnsTerrainQuadtree = true;
                return snapshot;
            },
            [](const std::string&) { return false; },
            [&prepared](TilesetTile&, double) {
                prepared = true;
                return true;
            },
            [&marked](const TileKey&) { marked = true; });

    EXPECT_EQ(outcome.issued, 1u);
    EXPECT_EQ(outcome.classifiedTerrainAvailabilityUpsample, 1u);
    EXPECT_EQ(outcome.issuedTerrainAvailabilityUpsample, 1u);
    EXPECT_EQ(outcome.classifiedContent, 0u);
    EXPECT_EQ(outcome.classifiedRasterDetailUpsample, 0u);
    EXPECT_FALSE(outcome.blockedByInflight);
    EXPECT_TRUE(prepared);
    EXPECT_TRUE(marked);
    EXPECT_EQ(provider.requestCount, 0);
    EXPECT_EQ(legacyProvider.requestCount, 0);
    ASSERT_TRUE(waitForUpsampleAsync([&]() {
        return lifecycle.counts().gltfTerrainUploads == 1u;
    }));
    EXPECT_EQ(lifecycle.counts().gltfTerrainUploads, 1u);
    EXPECT_EQ(lifecycle.counts().contentUploads, 0u);

    std::optional<PendingTileLoad> pending;
    {
        std::lock_guard<std::mutex> lock(lifecycle.mutex());
        pending =
            lifecycle.pendingLoads().takeHighestPriorityUpload(
                budget);
    }
    ASSERT_TRUE(pending.has_value());
    EXPECT_EQ(pending->domain, TileLoadDomain::TerrainContent);
    EXPECT_EQ(pending->result.status, TileLoadStatus::Renderable);
    EXPECT_TRUE(pending->content().hasGltfTerrainPayload());
    ASSERT_NE(pending->content().gltfModel, nullptr);
    ASSERT_TRUE(pending->content().metadata.rasterOverlayDetails.has_value());
    const RasterOverlayDetails& details =
        *pending->content().metadata.rasterOverlayDetails;
    EXPECT_EQ(
        details.textureCoordinateIDForProjection(
            RasterOverlayProjection::Geographic),
        0);
    ASSERT_NE(
        details.findRectangleForOverlayProjection(
            RasterOverlayProjection::Geographic),
        nullptr);
    EXPECT_EQ(
        details.boundingRegion.rectangle,
        childBounds);
}

// clip worker 化去重:worker 产出入 pendingLoads 后未被消费前,同一 child
// 再次请求应被 containsWorkForCacheKey 命中跳过——不重复派发 clip。
TEST(TileLoadSchedulerTest,
     TerrainUpsampleDeduplicatesQueuedClipOnResubmit) {
    TileLoadLifecycle lifecycle;
    FrameResourceBudgetConfig config;
    config.maxNetworkRequestsPerFrame = 4;
    config.maxNetworkInflight = 4;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);

    const TileKey parentKey{"test", 0, 0, 0};
    const TileKey childKey{"test", 1, 0, 0};
    const Rectangle parentBounds{-1.0, -0.5, 1.0, 0.5};
    const Rectangle childBounds{-1.0, -0.5, 0.0, 0.0};
    TilesetTile parent(parentKey, parentBounds);
    TilesetTile child(childKey, childBounds, &parent);
    child.content.markTerrainAvailabilityUpsample();
    parent.content.renderContent.setGltfContent(
        makeSchedulerQuadTerrainGltfModel(parentBounds));
    parent.content.renderContent.setTerrainRenderContent(true);
    parent.markRenderContentDone();

    TerrainQuadtreeContentProvider provider;
    const auto makeSnapshotFn =
        [&child](const TileKey&, const std::string&, TilesetTile*& tileState) {
            tileState = &child;
            TileLoadRequestSnapshot snapshot;
            snapshot.hasTile = true;
            snapshot.upsampleKind =
                TileContentUpsampleKind::TerrainAvailability;
            snapshot.contentProviderOwnsTerrainQuadtree = true;
            return snapshot;
        };
    const std::vector<TileLoadRequest> requests{
        TileLoadRequest{childKey, TileLoadPriorityGroup::Urgent, 100.0}};

    const TileLoadRequestOutcome first =
        TileLoadScheduler::requestMissingTiles(
            requests,
            TileLoadSchedulerInput{lifecycle, budget, &provider},
            cacheKeyForTile,
            makeSnapshotFn,
            [](const std::string&) { return false; },
            [](TilesetTile&, double) { return true; },
            [](const TileKey&) {});
    EXPECT_EQ(first.issued, 1u);
    ASSERT_TRUE(waitForUpsampleAsync([&]() {
        return lifecycle.counts().gltfTerrainUploads == 1u;
    }));

    // 结果仍在 pendingLoads(未消费)。同一 child 再请求 → 跳过、不重复派发。
    budget.beginFrame(2, config);
    const TileLoadRequestOutcome second =
        TileLoadScheduler::requestMissingTiles(
            requests,
            TileLoadSchedulerInput{lifecycle, budget, &provider},
            cacheKeyForTile,
            makeSnapshotFn,
            [](const std::string&) { return false; },
            [](TilesetTile&, double) { return true; },
            [](const TileKey&) {});
    EXPECT_EQ(second.issued, 0u);
    EXPECT_EQ(second.skippedAlreadyPending, 1u);
    EXPECT_EQ(lifecycle.counts().gltfTerrainUploads, 1u);
}

// clip worker 化析构安全:派发 clip 后立即析构,markDestroyingCancelAndWait
// 必须在有限时间内返回——UpsampleClipCompletionGuard 保证 worker exactly-once
// 完成 → completeTerrainRequest 排空 requestState → 析构等待放行,不死锁。
TEST(TileLoadSchedulerTest,
     TerrainUpsampleClipDoesNotDeadlockLifecycleDestruction) {
    TileLoadLifecycle lifecycle;
    FrameResourceBudgetConfig config;
    config.maxNetworkRequestsPerFrame = 4;
    config.maxNetworkInflight = 4;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);

    const TileKey parentKey{"test", 0, 0, 0};
    const TileKey childKey{"test", 1, 0, 0};
    const Rectangle parentBounds{-1.0, -0.5, 1.0, 0.5};
    const Rectangle childBounds{-1.0, -0.5, 0.0, 0.0};
    TilesetTile parent(parentKey, parentBounds);
    TilesetTile child(childKey, childBounds, &parent);
    child.content.markTerrainAvailabilityUpsample();
    parent.content.renderContent.setGltfContent(
        makeSchedulerQuadTerrainGltfModel(parentBounds));
    parent.content.renderContent.setTerrainRenderContent(true);
    parent.markRenderContentDone();

    TerrainQuadtreeContentProvider provider;
    const TileLoadRequestOutcome outcome =
        TileLoadScheduler::requestMissingTiles(
            {TileLoadRequest{childKey, TileLoadPriorityGroup::Urgent, 100.0}},
            TileLoadSchedulerInput{lifecycle, budget, &provider},
            cacheKeyForTile,
            [&child](
                const TileKey&, const std::string&, TilesetTile*& tileState) {
                tileState = &child;
                TileLoadRequestSnapshot snapshot;
                snapshot.hasTile = true;
                snapshot.upsampleKind =
                    TileContentUpsampleKind::TerrainAvailability;
                snapshot.contentProviderOwnsTerrainQuadtree = true;
                return snapshot;
            },
            [](const std::string&) { return false; },
            [](TilesetTile&, double) { return true; },
            [](const TileKey&) {});
    EXPECT_EQ(outcome.issued, 1u);

    std::atomic<bool> returned{false};
    std::thread destroyer([&]() {
        lifecycle.markDestroyingCancelAndWait();
        returned.store(true);
    });
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline && !returned.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_TRUE(returned.load());
    destroyer.join();
    EXPECT_EQ(lifecycle.pendingRequestCount(), 0);
}

TEST(TileLoadSchedulerTest,
     ContentLoadedRasterDetailParentQueuesGltfUpsampleLikeCesiumNative) {
    TileLoadLifecycle lifecycle;
    FrameResourceBudgetConfig config;
    config.maxNetworkRequestsPerFrame = 4;
    config.maxNetworkInflight = 4;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);

    const TileKey parentKey{"test", 0, 0, 0};
    const TileKey childKey{"test", 1, 0, 0};
    const Rectangle parentBounds{-1.0, -0.5, 1.0, 0.5};
    const Rectangle childBounds{-1.0, -0.5, 0.0, 0.0};
    TilesetTile parent(parentKey, parentBounds);
    TilesetTile child(childKey, childBounds, &parent);
    child.content.markRasterDetailUpsample(
        RasterOverlayProjection::Geographic);
    parent.content.renderContent.setGltfContent(
        makeSchedulerQuadTerrainGltfModel(parentBounds));
    parent.content.renderContent.setTerrainRenderContent(true);
    parent.content.renderContent.addGltfPrimitiveResource(
        GltfPrimitiveRenderResources{});
    parent.content.renderContent.markRenderContentReady();
    parent.markRenderContentLoaded();

    TerrainQuadtreeContentProvider provider;
    FanoutTerrainProvider legacyProvider;
    bool prepared = false;
    bool marked = false;

    const TileLoadRequestOutcome outcome =
        TileLoadScheduler::requestMissingTiles(
            {TileLoadRequest{
                childKey,
                TileLoadPriorityGroup::Urgent,
                100.0}},
            TileLoadSchedulerInput{
                lifecycle,
                budget,
                &provider},
            cacheKeyForTile,
            [&child](
                const TileKey&,
                const std::string&,
                TilesetTile*& tileState) {
                tileState = &child;
                TileLoadRequestSnapshot snapshot;
                snapshot.hasTile = true;
                snapshot.upsampleKind =
                    TileContentUpsampleKind::RasterDetail;
                snapshot.contentProviderOwnsTerrainQuadtree = true;
                return snapshot;
            },
            [](const std::string&) { return false; },
            [&prepared](TilesetTile& tile, double priority) {
                prepared = TileUpsampleSourcePreparer::prepareSourceTile(
                    tile,
                    priority,
                    false,
                    [](TilesetTile&) {},
                    [](const TileKey&, TileLoadPriorityGroup, double) {});
                return prepared;
            },
            [&marked](const TileKey&) { marked = true; });

    EXPECT_EQ(outcome.issued, 1u);
    EXPECT_EQ(outcome.classifiedRasterDetailUpsample, 1u);
    EXPECT_EQ(outcome.issuedRasterDetailUpsample, 1u);
    EXPECT_EQ(outcome.classifiedContent, 0u);
    EXPECT_EQ(outcome.classifiedTerrainAvailabilityUpsample, 0u);
    EXPECT_FALSE(outcome.blockedByInflight);
    EXPECT_TRUE(prepared);
    EXPECT_TRUE(marked);
    EXPECT_EQ(TileLoadState::ContentLoaded, parent.content.loadState);
    EXPECT_EQ(provider.requestCount, 0);
    EXPECT_EQ(legacyProvider.requestCount, 0);
    ASSERT_TRUE(waitForUpsampleAsync([&]() {
        return lifecycle.counts().gltfTerrainUploads == 1u;
    }));
    EXPECT_EQ(lifecycle.counts().gltfTerrainUploads, 1u);
    EXPECT_EQ(lifecycle.counts().contentUploads, 0u);

    std::optional<PendingTileLoad> pending;
    {
        std::lock_guard<std::mutex> lock(lifecycle.mutex());
        pending =
            lifecycle.pendingLoads().takeHighestPriorityUpload(
                budget);
    }
    ASSERT_TRUE(pending.has_value());
    EXPECT_EQ(pending->domain, TileLoadDomain::TerrainContent);
    EXPECT_EQ(pending->result.status, TileLoadStatus::Renderable);
    EXPECT_TRUE(pending->content().hasGltfTerrainPayload());
    ASSERT_NE(pending->content().gltfModel, nullptr);
    ASSERT_TRUE(pending->content().metadata.rasterOverlayDetails.has_value());
    EXPECT_EQ(
        childBounds,
        pending->content()
            .metadata
            .rasterOverlayDetails
            ->boundingRegion
            .rectangle);
}

TEST(
    TileLoadSchedulerTest,
    FailedGltfTerrainUpsampleQueuesFailedTerminalLikeCesiumNative) {
    TileLoadLifecycle lifecycle;
    FrameResourceBudgetConfig config;
    config.maxNetworkRequestsPerFrame = 4;
    config.maxNetworkInflight = 4;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);

    const TileKey parentKey{"Geographic-TMS", 0, 0, 0};
    const TileKey childKey{"Geographic-TMS", 1, 0, 0};
    const Rectangle parentBounds =
        Rectangle::fromDegrees(-180.0, -90.0, 180.0, 90.0);
    TilesetTile parent(parentKey, parentBounds);
    TilesetTile child(
        childKey,
        Rectangle::fromDegrees(-180.0, -90.0, 0.0, 0.0),
        &parent);
    parent.children.push_back(&child);
    child.content.markTerrainAvailabilityUpsample();
    auto parentModel = makeSchedulerQuadTerrainGltfModel(parentBounds);
    GltfPrimitive& primitive = parentModel->primitives.front();
    primitive.vertexTexCoords[0] = {
        std::array<float, 2>{0.75f, 0.75f},
        std::array<float, 2>{1.0f, 0.75f},
        std::array<float, 2>{0.75f, 1.0f},
        std::array<float, 2>{1.0f, 1.0f}};
    parent.content.renderContent.setGltfContent(std::move(parentModel));
    parent.content.renderContent.setTerrainRenderContent(true);
    parent.markRenderContentDone();

    bool marked = false;
    const TileLoadRequestOutcome outcome =
        TileLoadScheduler::requestMissingTiles(
            {TileLoadRequest{
                childKey,
                TileLoadPriorityGroup::Urgent,
                100.0}},
            TileLoadSchedulerInput{
                lifecycle,
                budget,
                nullptr},
            cacheKeyForTile,
            [&child](
                const TileKey&,
                const std::string&,
                TilesetTile*& tileState) {
                tileState = &child;
                TileLoadRequestSnapshot snapshot;
                snapshot.hasTile = true;
                snapshot.upsampleKind = TileContentUpsampleKind::TerrainAvailability;
                snapshot.loadState = child.content.loadState;
                return snapshot;
            },
            [](const std::string&) { return false; },
            [](TilesetTile&, double) {
                return true;
            },
            [&marked](const TileKey&) { marked = true; });

    EXPECT_EQ(1u, outcome.issued);
    EXPECT_FALSE(outcome.blockedByInflight);
    EXPECT_TRUE(marked);
    ASSERT_TRUE(waitForUpsampleAsync([&]() {
        return lifecycle.counts().gltfTerrainTerminalResults == 1u;
    }));
    EXPECT_EQ(1u, lifecycle.counts().gltfTerrainTerminalResults);
    EXPECT_EQ(0u, lifecycle.counts().gltfTerrainUploads);

    std::optional<PendingTileLoad> terminal;
    {
        std::lock_guard<std::mutex> lock(lifecycle.mutex());
        terminal =
            lifecycle.pendingLoads().takeHighestPriorityTerminalResult(
                budget);
    }
    ASSERT_TRUE(terminal.has_value());
    EXPECT_EQ(TileLoadDomain::TerrainContent, terminal->domain);
    EXPECT_EQ(TileLoadStatus::Failed, terminal->result.status);
}

TEST(
    TileLoadSchedulerTest,
    ProtectedUnloadingTerrainSourceCanFinishQueuedUpsampleLikeCesiumNative) {
    const TileKey parentKey{"test", 0, 0, 0};
    const TileKey childKey{"test", 1, 0, 0};
    const Rectangle parentBounds{-1.0, -0.5, 1.0, 0.5};
    const Rectangle childBounds{-1.0, -0.5, 0.0, 0.0};
    TilesetTile parent(parentKey, parentBounds);
    TilesetTile child(childKey, childBounds, &parent);
    parent.children.push_back(&child);

    child.content.markTerrainAvailabilityUpsample();
    child.content.loadState = TileLoadState::ContentLoading;
    parent.content.renderContent.setGltfContent(
        makeSchedulerQuadTerrainGltfModel(parentBounds));
    parent.content.renderContent.setTerrainRenderContent(true);
    parent.content.contentKind = TileContentKind::Render;
    parent.content.loadState = TileLoadState::Unloading;

    EXPECT_TRUE(
        TileGltfTerrainUpsampledChildMaterializer::canCreateLoadResult(
            child));
    std::optional<TileLoadResult> result =
        TileGltfTerrainUpsampledChildMaterializer::createLoadResult(child);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(TileLoadStatus::Renderable, result->status);
    EXPECT_TRUE(result->content.hasGltfTerrainPayload());
    ASSERT_NE(nullptr, result->content.gltfModel);
    ASSERT_TRUE(result->content.metadata.rasterOverlayDetails.has_value());
    EXPECT_EQ(
        childBounds,
        result->content.metadata.rasterOverlayDetails->boundingRegion
            .rectangle);
}

TEST(
    TileLoadSchedulerTest,
    ContentOwnedTerrainProviderSuppressesLegacyTerrainDispatchEvenWithBadSnapshot) {
    TileLoadLifecycle lifecycle;
    FrameResourceBudgetConfig config;
    config.maxNetworkRequestsPerFrame = 4;
    config.maxNetworkInflight = 4;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);

    const TileKey key{"test", 0, 0, 0};
    TerrainQuadtreeContentProvider contentProvider;
    DeferredTerrainProvider legacyProvider;
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
                &contentProvider},
            cacheKeyForTile,
            [](const TileKey&,
               const std::string&,
               TilesetTile*& tileState) {
                tileState = nullptr;
                TileLoadRequestSnapshot snapshot;
                return snapshot;
            },
            [](const std::string&) { return false; },
            [](TilesetTile&, double) { return false; },
            [&marked](const TileKey&) { marked = true; });

    EXPECT_EQ(0u, outcome.issued);
    EXPECT_FALSE(outcome.blockedByInflight);
    EXPECT_EQ(0, contentProvider.requestCount);
    EXPECT_EQ(0, legacyProvider.requestCount);
    EXPECT_FALSE(marked);
    EXPECT_EQ(0u, lifecycle.pendingRequestCount());
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
                nullptr},
            cacheKeyForTile,
            [&planned](
                const TileKey&,
                const std::string&,
                TilesetTile*& tileState) {
                planned = true;
                tileState = nullptr;
                TileLoadRequestSnapshot snapshot;
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

TEST(TileLoadSchedulerTest,
     SortsTerrainContentUpsampleButDoesNotQueueLegacyDomainWithoutGltfSource) {
    TileLoadLifecycle lifecycle;
    FrameResourceBudgetConfig config;
    config.maxNetworkRequestsPerFrame = 4;
    config.maxNetworkInflight = 4;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);

    const TileKey normalKey{"test", 1, 0, 0};
    const TileKey urgentKey{"test", 1, 1, 0};
    TilesetTile normalTile(normalKey, Rectangle{});
    TilesetTile urgentTile(urgentKey, Rectangle{});
    normalTile.content.markTerrainAvailabilityUpsample();
    urgentTile.content.markTerrainAvailabilityUpsample();
    std::vector<int> prepareOrder;
    std::vector<int> markedOrder;

    const TileLoadRequestOutcome outcome =
        TileLoadScheduler::requestMissingTiles(
            {
                TileLoadRequest{
                    normalKey,
                    TileLoadPriorityGroup::Normal,
                    0.0},
                TileLoadRequest{
                    urgentKey,
                    TileLoadPriorityGroup::Urgent,
                    100.0},
            },
            TileLoadSchedulerInput{
                lifecycle,
                budget,
                nullptr},
            cacheKeyForTile,
            [&urgentKey, &normalTile, &urgentTile](
                const TileKey& key,
                const std::string&,
                TilesetTile*& tileState) {
                tileState = key == urgentKey ? &urgentTile : &normalTile;
                TileLoadRequestSnapshot snapshot;
                snapshot.hasTile = true;
                snapshot.upsampleKind = TileContentUpsampleKind::TerrainAvailability;
                return snapshot;
            },
            [](const std::string&) { return false; },
            [&prepareOrder](TilesetTile& tile, double) {
                prepareOrder.push_back(tile.key.x);
                return true;
            },
            [&markedOrder](const TileKey& key) {
                markedOrder.push_back(key.x);
            });

    EXPECT_EQ(outcome.issued, 0u);
    EXPECT_FALSE(outcome.blockedByInflight);
    ASSERT_EQ(prepareOrder.size(), 2u);
    EXPECT_EQ(prepareOrder[0], urgentKey.x);
    EXPECT_EQ(prepareOrder[1], normalKey.x);
    EXPECT_TRUE(markedOrder.empty());
    EXPECT_EQ(lifecycle.counts().gltfTerrainUploads, 0u);
}

TEST(TileLoadSchedulerTest, ContinuesAfterUpsampleSourceWait) {
    TileLoadLifecycle lifecycle;
    FrameResourceBudgetConfig config;
    config.maxNetworkRequestsPerFrame = 4;
    config.maxNetworkInflight = 4;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);

    const TileKey waitingUpsampleKey{"test", 2, 0, 0};
    const TileKey loadableTerrainKey{"test", 1, 1, 0};
    TilesetTile waitingTile(waitingUpsampleKey, Rectangle{});
    waitingTile.content.markTerrainAvailabilityUpsample();
    DeferredTerrainProvider provider;
    std::vector<int> plannedLevels;
    std::vector<int> preparedLevels;
    std::vector<int> markedLevels;

    const TileLoadRequestOutcome outcome =
        TileLoadScheduler::requestMissingTiles(
            {
                TileLoadRequest{
                    loadableTerrainKey,
                    TileLoadPriorityGroup::Normal,
                    50.0},
                TileLoadRequest{
                    waitingUpsampleKey,
                    TileLoadPriorityGroup::Urgent,
                    100.0},
            },
            TileLoadSchedulerInput{
                lifecycle,
                budget,
                nullptr},
            cacheKeyForTile,
            [&waitingUpsampleKey, &waitingTile, &plannedLevels](
                const TileKey& key,
                const std::string&,
                TilesetTile*& tileState) {
                plannedLevels.push_back(key.z);
                TileLoadRequestSnapshot snapshot;
                if (key == waitingUpsampleKey) {
                    tileState = &waitingTile;
                    snapshot.hasTile = true;
                    snapshot.upsampleKind = TileContentUpsampleKind::TerrainAvailability;
                } else {
                    tileState = nullptr;
                }
                return snapshot;
            },
            [](const std::string&) { return false; },
            [&preparedLevels](TilesetTile& tile, double) {
                preparedLevels.push_back(tile.key.z);
                return false;
            },
            [&markedLevels](const TileKey& key) {
                markedLevels.push_back(key.z);
            });

    ASSERT_EQ(plannedLevels.size(), 2u);
    ASSERT_EQ(preparedLevels.size(), 1u);
    EXPECT_TRUE(markedLevels.empty());
    EXPECT_EQ(outcome.issued, 0u);
    EXPECT_FALSE(outcome.blockedByInflight);
    EXPECT_EQ(plannedLevels[0], waitingUpsampleKey.z);
    EXPECT_EQ(plannedLevels[1], loadableTerrainKey.z);
    EXPECT_EQ(preparedLevels.front(), waitingUpsampleKey.z);
    EXPECT_EQ(provider.requestCount, 0);
    EXPECT_EQ(lifecycle.pendingRequestCount(), 0u);
    EXPECT_EQ(lifecycle.counts().gltfTerrainUploads, 0u);
}

TEST(TileLoadSchedulerTest,
     GltfTerrainUpsampleSourcePreparationQueuesDirectParentLikeCesiumNative) {
    TilesetTile grandparent(
        TileKey{"Geographic-TMS", 0, 0, 0},
        Rectangle::fromDegrees(-180.0, -90.0, 0.0, 90.0));
    TilesetTile parent(
        TileKey{"Geographic-TMS", 1, 0, 0},
        Rectangle::fromDegrees(-180.0, -90.0, -90.0, 0.0),
        &grandparent);
    TilesetTile child(
        TileKey{"Geographic-TMS", 2, 0, 0},
        Rectangle::fromDegrees(-180.0, -90.0, -135.0, -45.0),
        &parent);
    grandparent.children.push_back(&parent);
    parent.children.push_back(&child);
    child.content.markTerrainAvailabilityUpsample();

    grandparent.content.renderContent.prepareGltfContent(
        makeSchedulerQuadTerrainGltfModel(grandparent.bounds),
        Mat4::identity());
    grandparent.content.renderContent.setTerrainRenderContent(true);
    grandparent.markRenderContentDone();

    std::vector<TileKey> queuedKeys;
    bool ensured = false;
    const bool ready = TileUpsampleSourcePreparer::prepareSourceTile(
        child,
        12.0,
        false,
        [&ensured](TilesetTile&) {
            ensured = true;
        },
        [&queuedKeys](const TileKey& key,
                      TileLoadPriorityGroup,
                      double) {
            queuedKeys.push_back(key);
        });

    EXPECT_FALSE(ready);
    EXPECT_FALSE(ensured);
    ASSERT_EQ(1u, queuedKeys.size());
    EXPECT_EQ(parent.key, queuedKeys.front());
}

TEST(
    TileLoadSchedulerTest,
    GltfTerrainUpsampleSourcePreparationDoesNotQueueGrandparentLikeCesiumNative) {
    TilesetTile grandparent(
        TileKey{"Geographic-TMS", 0, 0, 0},
        Rectangle::fromDegrees(-180.0, -90.0, 0.0, 90.0));
    TilesetTile parent(
        TileKey{"Geographic-TMS", 1, 0, 0},
        Rectangle::fromDegrees(-180.0, -90.0, -90.0, 0.0),
        &grandparent);
    TilesetTile child(
        TileKey{"Geographic-TMS", 2, 0, 0},
        Rectangle::fromDegrees(-180.0, -90.0, -135.0, -45.0),
        &parent);
    grandparent.children.push_back(&parent);
    parent.children.push_back(&child);
    child.content.markTerrainAvailabilityUpsample();

    grandparent.content.loadState = TileLoadState::Unloaded;
    parent.content.contentKind = TileContentKind::Render;
    parent.content.loadState = TileLoadState::ContentLoaded;

    std::vector<TileKey> ensuredKeys;
    std::vector<TileKey> queuedKeys;
    const bool ready = TileUpsampleSourcePreparer::prepareSourceTile(
        child,
        12.0,
        false,
        [&ensuredKeys](TilesetTile& tile) {
            ensuredKeys.push_back(tile.key);
        },
        [&queuedKeys](const TileKey& key,
                      TileLoadPriorityGroup,
                      double) {
            queuedKeys.push_back(key);
        });

    EXPECT_FALSE(ready);
    ASSERT_EQ(1u, ensuredKeys.size());
    EXPECT_EQ(parent.key, ensuredKeys.front());
    EXPECT_TRUE(queuedKeys.empty());
}

TEST(TileLoadSchedulerTest, ContinuesAfterMissingUpsampleTileState) {
    TileLoadLifecycle lifecycle;
    FrameResourceBudgetConfig config;
    config.maxNetworkRequestsPerFrame = 4;
    config.maxNetworkInflight = 4;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);

    const TileKey missingTileStateKey{"test", 2, 0, 0};
    const TileKey readyUpsampleKey{"test", 2, 1, 0};
    TilesetTile readyTile(readyUpsampleKey, Rectangle{});
    readyTile.content.markTerrainAvailabilityUpsample();
    std::vector<int> plannedColumns;
    std::vector<int> preparedColumns;
    std::vector<int> markedColumns;

    const TileLoadRequestOutcome outcome =
        TileLoadScheduler::requestMissingTiles(
            {
                TileLoadRequest{
                    readyUpsampleKey,
                    TileLoadPriorityGroup::Normal,
                    50.0},
                TileLoadRequest{
                    missingTileStateKey,
                    TileLoadPriorityGroup::Urgent,
                    100.0},
            },
            TileLoadSchedulerInput{
                lifecycle,
                budget,
                nullptr},
            cacheKeyForTile,
            [&missingTileStateKey, &readyTile, &plannedColumns](
                const TileKey& key,
                const std::string&,
                TilesetTile*& tileState) {
                plannedColumns.push_back(key.x);
                TileLoadRequestSnapshot snapshot;
                snapshot.hasTile = true;
                snapshot.upsampleKind = TileContentUpsampleKind::TerrainAvailability;
                tileState = key == missingTileStateKey ? nullptr : &readyTile;
                return snapshot;
            },
            [](const std::string&) { return false; },
            [&preparedColumns](TilesetTile& tile, double) {
                preparedColumns.push_back(tile.key.x);
                return true;
            },
            [&markedColumns](const TileKey& key) {
                markedColumns.push_back(key.x);
            });

    ASSERT_EQ(plannedColumns.size(), 2u);
    ASSERT_EQ(preparedColumns.size(), 1u);
    EXPECT_TRUE(markedColumns.empty());
    EXPECT_EQ(outcome.issued, 0u);
    EXPECT_FALSE(outcome.blockedByInflight);
    EXPECT_EQ(plannedColumns[0], missingTileStateKey.x);
    EXPECT_EQ(plannedColumns[1], readyUpsampleKey.x);
    EXPECT_EQ(preparedColumns.front(), readyUpsampleKey.x);
    EXPECT_EQ(lifecycle.counts().gltfTerrainUploads, 0u);
}

TEST(TileLoadSchedulerTest,
     PermanentlyFailedTerrainUpsampleDoesNotPrepareOrRetryLikeCesiumNative) {
    TileLoadLifecycle lifecycle;
    FrameResourceBudgetConfig config;
    config.maxNetworkRequestsPerFrame = 4;
    config.maxNetworkInflight = 4;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);

    const TileKey failedUpsampleKey{"test", 2, 0, 0};
    TilesetTile failedTile(failedUpsampleKey, Rectangle{});
    failedTile.content.markTerrainAvailabilityUpsample();
    failedTile.content.loadState = TileLoadState::Failed;

    bool snapshotBuilt = false;
    bool prepared = false;
    bool markedLoading = false;

    const TileLoadRequestOutcome outcome =
        TileLoadScheduler::requestMissingTiles(
            {TileLoadRequest{
                failedUpsampleKey,
                TileLoadPriorityGroup::Normal,
                10.0}},
            TileLoadSchedulerInput{
                lifecycle,
                budget,
                nullptr},
            cacheKeyForTile,
            [&failedUpsampleKey, &failedTile, &snapshotBuilt](
                const TileKey& key,
                const std::string&,
                TilesetTile*& tileState) {
                EXPECT_EQ(failedUpsampleKey, key);
                snapshotBuilt = true;
                tileState = &failedTile;
                TileLoadRequestSnapshot snapshot;
                snapshot.hasTile = true;
                snapshot.upsampleKind = TileContentUpsampleKind::TerrainAvailability;
                snapshot.loadState = failedTile.content.loadState;
                return snapshot;
            },
            [](const std::string&) { return false; },
            [&prepared](TilesetTile&, double) {
                prepared = true;
                return true;
            },
            [&markedLoading](const TileKey&) {
                markedLoading = true;
            });

    EXPECT_TRUE(snapshotBuilt);
    EXPECT_FALSE(prepared);
    EXPECT_FALSE(markedLoading);
    EXPECT_EQ(TileLoadState::Failed, failedTile.content.loadState);
    EXPECT_EQ(0u, outcome.issued);
    EXPECT_FALSE(outcome.blockedByInflight);
    EXPECT_EQ(0u, lifecycle.counts().gltfTerrainTerminalResults);
    EXPECT_EQ(0u, lifecycle.counts().gltfTerrainUploads);
}

TEST(TileLoadSchedulerTest, SkipsEmptyUpsampledCacheKey) {
    TileLoadLifecycle lifecycle;
    FrameResourceBudgetConfig config;
    config.maxNetworkRequestsPerFrame = 4;
    config.maxNetworkInflight = 4;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);

    const TileKey key{"test", 1, 0, 0};
    TilesetTile tile(key, Rectangle{});
    tile.content.markTerrainAvailabilityUpsample();
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
                nullptr},
            cacheKeyForTile,
            [&tile](
                const TileKey&,
                const std::string&,
                TilesetTile*& tileState) {
                tileState = &tile;
                TileLoadRequestSnapshot snapshot;
                snapshot.hasTile = true;
                snapshot.upsampleKind = TileContentUpsampleKind::TerrainAvailability;
                return snapshot;
            },
            [](const std::string&) { return true; },
            [&prepared](TilesetTile&, double) {
                prepared = true;
                return true;
            },
            [&marked](const TileKey&) { marked = true; });

    EXPECT_EQ(outcome.issued, 0u);
    EXPECT_FALSE(outcome.blockedByInflight);
    EXPECT_FALSE(prepared);
    EXPECT_FALSE(marked);
    EXPECT_FALSE(lifecycle.hasPendingWork());
}

TEST(TileLoadSchedulerTest, SkipsPendingCacheKeyBeforeUpsamplePreparation) {
    TileLoadLifecycle lifecycle;
    FrameResourceBudgetConfig config;
    config.maxNetworkRequestsPerFrame = 4;
    config.maxNetworkInflight = 4;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);

    const TileKey key{"test", 1, 0, 0};
    TilesetTile tile(key, Rectangle{});
    tile.content.markTerrainAvailabilityUpsample();
    const std::string cacheKey = cacheKeyForTile(key);
    {
        std::lock_guard<std::mutex> lock(lifecycle.mutex());
        lifecycle.pendingLoads().addTerminalResult(PendingTileLoad{TileLoadDomain::TerrainContent,
                key,
                cacheKey,
                TileLoadPriorityGroup::Normal,
                0.0,
                TileLoadStatus::RetryLater});
    }

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
                nullptr},
            cacheKeyForTile,
            [&tile](
                const TileKey&,
                const std::string&,
                TilesetTile*& tileState) {
                tileState = &tile;
                TileLoadRequestSnapshot snapshot;
                snapshot.hasTile = true;
                snapshot.upsampleKind = TileContentUpsampleKind::TerrainAvailability;
                return snapshot;
            },
            [](const std::string&) { return false; },
            [&prepared](TilesetTile&, double) {
                prepared = true;
                return true;
            },
            [&marked](const TileKey&) { marked = true; });

    EXPECT_EQ(outcome.issued, 0u);
    EXPECT_FALSE(outcome.blockedByInflight);
    EXPECT_FALSE(prepared);
    EXPECT_FALSE(marked);
    EXPECT_EQ(lifecycle.counts().gltfTerrainTerminalResults, 1u);
    EXPECT_FALSE(lifecycle.containsWorkForCacheKey("unexpected"));
}

TEST(TileLoadSchedulerTest, SkipsPendingUploadBeforeSnapshot) {
    TileLoadLifecycle lifecycle;
    FrameResourceBudgetConfig config;
    config.maxNetworkInflight = 4;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);

    const TileKey key{"test", 1, 0, 0};
    const std::string cacheKey = cacheKeyForTile(key);
    {
        std::lock_guard<std::mutex> lock(lifecycle.mutex());
        lifecycle.pendingLoads().addUpload(PendingTileLoad{TileLoadDomain::TerrainContent,
            key,
            cacheKey,
            TileLoadPriorityGroup::Normal,
            0.0,
            TileLoadResult::createRenderableGltfTerrain(std::make_unique<GltfModel>())});
    }

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
            [&marked](const TileKey&) { marked = true; });

    EXPECT_EQ(outcome.issued, 0u);
    EXPECT_FALSE(outcome.blockedByInflight);
    EXPECT_FALSE(planned);
    EXPECT_FALSE(marked);
    EXPECT_TRUE(lifecycle.containsWorkForCacheKey(cacheKey));
}

TEST(TileLoadSchedulerTest, SkipsClaimedUploadBeforeSnapshot) {
    TileLoadLifecycle lifecycle;
    FrameResourceBudgetConfig config;
    config.maxNetworkInflight = 4;
    config.maxMainThreadFinalizesPerFrame = 1;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);

    const TileKey key{"test", 1, 0, 0};
    const std::string cacheKey = cacheKeyForTile(key);
    {
        std::lock_guard<std::mutex> lock(lifecycle.mutex());
        lifecycle.pendingLoads().addUpload(PendingTileLoad{TileLoadDomain::TerrainContent,
            key,
            cacheKey,
            TileLoadPriorityGroup::Normal,
            0.0,
            TileLoadResult::createRenderableGltfTerrain(std::make_unique<GltfModel>())});
        ASSERT_TRUE(lifecycle.pendingLoads()
                        .takeHighestPriorityUpload(budget)
                        .has_value());
    }

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
            [&marked](const TileKey&) { marked = true; });

    EXPECT_EQ(outcome.issued, 0u);
    EXPECT_FALSE(outcome.blockedByInflight);
    EXPECT_FALSE(planned);
    EXPECT_FALSE(marked);
    EXPECT_TRUE(lifecycle.containsWorkForCacheKey(cacheKey));
    EXPECT_TRUE(lifecycle.hasPendingWork());
}

TEST(TileLoadSchedulerTest, SkipsPendingTerminalBeforeSnapshot) {
    TileLoadLifecycle lifecycle;
    FrameResourceBudgetConfig config;
    config.maxNetworkInflight = 4;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);

    const TileKey key{"test", 1, 0, 0};
    const std::string cacheKey = cacheKeyForTile(key);
    {
        std::lock_guard<std::mutex> lock(lifecycle.mutex());
        lifecycle.pendingLoads().addTerminalResult(PendingTileLoad{TileLoadDomain::Content,
                key,
                cacheKey,
                TileLoadPriorityGroup::Normal,
                0.0,
                TileLoadStatus::RetryLater});
    }

    bool planned = false;
    bool emptyChecked = false;
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
            [&emptyChecked](const std::string&) {
                emptyChecked = true;
                return false;
            },
            [](TilesetTile&, double) { return false; },
            [&marked](const TileKey&) { marked = true; });

    EXPECT_EQ(outcome.issued, 0u);
    EXPECT_FALSE(outcome.blockedByInflight);
    EXPECT_FALSE(planned);
    EXPECT_FALSE(emptyChecked);
    EXPECT_FALSE(marked);
    EXPECT_TRUE(lifecycle.containsWorkForCacheKey(cacheKey));
    EXPECT_EQ(lifecycle.counts().contentTerminalResults, 1u);
}

TEST(TileLoadSchedulerTest, SkipsInflightRequestBeforeSnapshot) {
    TileLoadLifecycle lifecycle;
    FrameResourceBudgetConfig config;
    config.maxNetworkInflight = 4;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);

    const TileKey key{"test", 1, 0, 0};
    const std::string cacheKey = cacheKeyForTile(key);
    CancellationToken token;
    {
        std::lock_guard<std::mutex> lock(lifecycle.mutex());
        ASSERT_TRUE(lifecycle.requestState().beginTerrainRequest(
            cacheKey,
            token));
    }

    bool planned = false;
    bool emptyChecked = false;
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
            [&emptyChecked](const std::string&) {
                emptyChecked = true;
                return false;
            },
            [&prepared](TilesetTile&, double) {
                prepared = true;
                return true;
            },
            [&marked](const TileKey&) { marked = true; });

    EXPECT_EQ(outcome.issued, 0u);
    EXPECT_FALSE(outcome.blockedByInflight);
    EXPECT_FALSE(planned);
    EXPECT_FALSE(emptyChecked);
    EXPECT_FALSE(prepared);
    EXPECT_FALSE(marked);
    EXPECT_TRUE(lifecycle.containsWorkForCacheKey(cacheKey));

    {
        std::lock_guard<std::mutex> lock(lifecycle.mutex());
        lifecycle.requestState().completeTerrainRequest(cacheKey);
    }
}

TEST(TileLoadSchedulerTest, StopsDuringDestroyBeforePlanning) {
    TileLoadLifecycle lifecycle;
    FrameResourceBudgetConfig config;
    config.maxNetworkInflight = 4;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);
    {
        std::lock_guard<std::mutex> lock(lifecycle.mutex());
        lifecycle.requestState().markDestroyingAndCancelRequests();
    }

    bool cacheKeyRequested = false;
    bool planned = false;
    bool emptyChecked = false;
    bool prepared = false;
    bool marked = false;

    const TileLoadRequestOutcome outcome =
        TileLoadScheduler::requestMissingTiles(
            {TileLoadRequest{
                TileKey{"test", 0, 0, 0},
                TileLoadPriorityGroup::Urgent,
                100.0}},
            TileLoadSchedulerInput{
                lifecycle,
                budget,
                nullptr},
            [&cacheKeyRequested](const TileKey&) {
                cacheKeyRequested = true;
                return std::string{"unexpected"};
            },
            [&planned](
                const TileKey&,
                const std::string&,
                TilesetTile*& tileState) {
                planned = true;
                tileState = nullptr;
                return TileLoadRequestSnapshot{};
            },
            [&emptyChecked](const std::string&) {
                emptyChecked = true;
                return false;
            },
            [&prepared](TilesetTile&, double) {
                prepared = true;
                return true;
            },
            [&marked](const TileKey&) { marked = true; });

    EXPECT_EQ(outcome.issued, 0u);
    EXPECT_FALSE(outcome.blockedByInflight);
    EXPECT_FALSE(cacheKeyRequested);
    EXPECT_FALSE(planned);
    EXPECT_FALSE(emptyChecked);
    EXPECT_FALSE(prepared);
    EXPECT_FALSE(marked);
}

TEST(TileLoadSchedulerTest, SkipsDispatcherDuplicateAfterPlanning) {
    TileLoadLifecycle lifecycle;
    FrameResourceBudgetConfig config;
    config.maxNetworkInflight = 4;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);

    const TileKey key{"test", 0, 0, 0};
    const std::string cacheKey = cacheKeyForTile(key);
    CountingContentProvider provider;
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
                &provider},
            cacheKeyForTile,
            [&lifecycle, &planned, &key, &cacheKey](
                const TileKey&,
                const std::string&,
                TilesetTile*& tileState) {
                planned = true;
                tileState = nullptr;
                {
                    std::lock_guard<std::mutex> lock(lifecycle.mutex());
                    lifecycle.pendingLoads().addTerminalResult(PendingTileLoad{TileLoadDomain::Content,
                            key,
                            cacheKey,
                            TileLoadPriorityGroup::Normal,
                            0.0,
                            TileLoadStatus::RetryLater});
                }
                TileLoadRequestSnapshot snapshot;
                snapshot.contentProviderSupportsTile = true;
                return snapshot;
            },
            [](const std::string&) { return false; },
            [](TilesetTile&, double) { return false; },
            [&marked](const TileKey&) { marked = true; });

    EXPECT_EQ(outcome.issued, 0u);
    EXPECT_FALSE(outcome.blockedByInflight);
    EXPECT_TRUE(planned);
    EXPECT_FALSE(marked);
    EXPECT_EQ(provider.requestCount, 0);
    EXPECT_EQ(lifecycle.counts().contentTerminalResults, 1u);
}

TEST(TileLoadSchedulerTest, SkipsTerrainDispatcherDuplicateAfterPlanning) {
    TileLoadLifecycle lifecycle;
    FrameResourceBudgetConfig config;
    config.maxNetworkInflight = 4;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);

    const TileKey key{"test", 0, 0, 0};
    const std::string cacheKey = cacheKeyForTile(key);
    DeferredTerrainProvider provider;
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
                nullptr},
            cacheKeyForTile,
            [&lifecycle, &planned, &key, &cacheKey](
                const TileKey&,
                const std::string&,
                TilesetTile*& tileState) {
                planned = true;
                tileState = nullptr;
                {
                    std::lock_guard<std::mutex> lock(lifecycle.mutex());
                    lifecycle.pendingLoads().addTerminalResult(PendingTileLoad{TileLoadDomain::TerrainContent,
                            key,
                            cacheKey,
                            TileLoadPriorityGroup::Normal,
                            0.0,
                            TileLoadStatus::RetryLater});
                }
                TileLoadRequestSnapshot snapshot;
                return snapshot;
            },
            [](const std::string&) { return false; },
            [](TilesetTile&, double) { return false; },
            [&marked](const TileKey&) { marked = true; });

    EXPECT_EQ(outcome.issued, 0u);
    EXPECT_FALSE(outcome.blockedByInflight);
    EXPECT_TRUE(planned);
    EXPECT_FALSE(marked);
    EXPECT_EQ(provider.requestCount, 0);
    EXPECT_EQ(lifecycle.counts().gltfTerrainTerminalResults, 1u);
}

TEST(TileLoadSchedulerTest, ContinuesScanningAfterDispatchBudgetBlock) {
    TileLoadLifecycle lifecycle;
    FrameResourceBudgetConfig config;
    config.maxNetworkRequestsPerFrame = 1;
    config.maxNetworkInflight = 4;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);

    const TileKey firstKey{"test", 0, 0, 0};
    const TileKey blockedKey{"test", 0, 1, 0};
    const TileKey skippedKey{"test", 0, 2, 0};
    CountingContentProvider provider;
    std::vector<int> plannedKeys;
    std::vector<int> markedKeys;

    const TileLoadRequestOutcome outcome =
        TileLoadScheduler::requestMissingTiles(
            {
                TileLoadRequest{
                    skippedKey,
                    TileLoadPriorityGroup::Preload,
                    10.0},
                TileLoadRequest{
                    blockedKey,
                    TileLoadPriorityGroup::Normal,
                    50.0},
                TileLoadRequest{
                    firstKey,
                    TileLoadPriorityGroup::Urgent,
                    100.0},
            },
            TileLoadSchedulerInput{
                lifecycle,
                budget,
                &provider},
            cacheKeyForTile,
            [&plannedKeys](
                const TileKey& key,
                const std::string&,
                TilesetTile*& tileState) {
                plannedKeys.push_back(key.x);
                tileState = nullptr;
                TileLoadRequestSnapshot snapshot;
                snapshot.contentProviderSupportsTile = true;
                return snapshot;
            },
            [](const std::string&) { return false; },
            [](TilesetTile&, double) { return false; },
            [&markedKeys](const TileKey& key) {
                markedKeys.push_back(key.x);
            });

    ASSERT_EQ(plannedKeys.size(), 3u);
    ASSERT_EQ(markedKeys.size(), 1u);
    EXPECT_EQ(outcome.issued, 1u);
    EXPECT_FALSE(outcome.blockedByInflight);
    EXPECT_EQ(outcome.stoppedAtDispatch, 0u);
    EXPECT_EQ(provider.requestCount, 1);
    EXPECT_EQ(plannedKeys[0], firstKey.x);
    EXPECT_EQ(plannedKeys[1], blockedKey.x);
    EXPECT_EQ(plannedKeys[2], skippedKey.x);
    EXPECT_EQ(markedKeys[0], firstKey.x);
}

TEST(TileLoadSchedulerTest, IgnoresLegacyTerrainRequestsWithoutBudgetBlock) {
    TileLoadLifecycle lifecycle;
    FrameResourceBudgetConfig config;
    config.maxNetworkRequestsPerFrame = 1;
    config.maxNetworkInflight = 4;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);

    const TileKey firstKey{"test", 0, 0, 0};
    const TileKey blockedKey{"test", 0, 1, 0};
    const TileKey skippedKey{"test", 0, 2, 0};
    DeferredTerrainProvider provider;
    std::vector<int> plannedKeys;
    std::vector<int> markedKeys;

    const TileLoadRequestOutcome outcome =
        TileLoadScheduler::requestMissingTiles(
            {
                TileLoadRequest{
                    skippedKey,
                    TileLoadPriorityGroup::Preload,
                    10.0},
                TileLoadRequest{
                    blockedKey,
                    TileLoadPriorityGroup::Normal,
                    50.0},
                TileLoadRequest{
                    firstKey,
                    TileLoadPriorityGroup::Urgent,
                    100.0},
            },
            TileLoadSchedulerInput{
                lifecycle,
                budget,
                nullptr},
            cacheKeyForTile,
            [&plannedKeys](
                const TileKey& key,
                const std::string&,
                TilesetTile*& tileState) {
                plannedKeys.push_back(key.x);
                tileState = nullptr;
                TileLoadRequestSnapshot snapshot;
                return snapshot;
            },
            [](const std::string&) { return false; },
            [](TilesetTile&, double) { return false; },
            [&markedKeys](const TileKey& key) {
                markedKeys.push_back(key.x);
            });

    ASSERT_EQ(plannedKeys.size(), 3u);
    EXPECT_TRUE(markedKeys.empty());
    EXPECT_EQ(outcome.issued, 0u);
    EXPECT_FALSE(outcome.blockedByInflight);
    EXPECT_EQ(provider.requestCount, 0);
    EXPECT_EQ(plannedKeys[0], firstKey.x);
    EXPECT_EQ(plannedKeys[1], blockedKey.x);
    EXPECT_EQ(plannedKeys[2], skippedKey.x);
}

TEST(TileLoadSchedulerTest, ContentThenTerrainShareTerrainContentDispatchBudget) {
    TileLoadLifecycle lifecycle;
    FrameResourceBudgetConfig config;
    config.maxNetworkRequestsPerFrame = 4;
    config.maxTerrainContentNetworkRequestsPerFrame = 1;
    config.maxNetworkInflight = 4;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);

    const TileKey contentKey{"content", 0, 0, 0};
    const TileKey terrainKey{"test", 0, 1, 0};
    const TileKey skippedKey{"test", 0, 2, 0};
    CountingContentProvider contentProvider;
    DeferredTerrainProvider terrainProvider;
    std::vector<std::string> plannedKeys;
    std::vector<std::string> markedKeys;

    const TileLoadRequestOutcome outcome =
        TileLoadScheduler::requestMissingTiles(
            {
                TileLoadRequest{
                    skippedKey,
                    TileLoadPriorityGroup::Preload,
                    10.0},
                TileLoadRequest{
                    terrainKey,
                    TileLoadPriorityGroup::Normal,
                    50.0},
                TileLoadRequest{
                    contentKey,
                    TileLoadPriorityGroup::Urgent,
                    100.0},
            },
            TileLoadSchedulerInput{
                lifecycle,
                budget,
                &contentProvider},
            cacheKeyForTile,
            [&plannedKeys](
                const TileKey& key,
                const std::string&,
                TilesetTile*& tileState) {
                plannedKeys.push_back(
                    key.schemeId.str() + ":" + std::to_string(key.x));
                tileState = nullptr;
                TileLoadRequestSnapshot snapshot;
                if (key.schemeId == "content") {
                    snapshot.contentProviderSupportsTile = true;
                } else {
                }
                return snapshot;
            },
            [](const std::string&) { return false; },
            [](TilesetTile&, double) { return false; },
            [&markedKeys](const TileKey& key) {
                markedKeys.push_back(
                    key.schemeId.str() + ":" + std::to_string(key.x));
            });

    ASSERT_EQ(plannedKeys.size(), 3u);
    ASSERT_EQ(markedKeys.size(), 1u);
    EXPECT_EQ(outcome.issued, 1u);
    EXPECT_FALSE(outcome.blockedByInflight);
    EXPECT_EQ(contentProvider.requestCount, 1);
    EXPECT_EQ(terrainProvider.requestCount, 0);
    EXPECT_EQ(budget.terrainContentNetworkRequestsIssued(), 1u);
    EXPECT_EQ(budget.contentNetworkRequestsIssued(), 1u);
    EXPECT_EQ(budget.networkRequestsIssued(), 1u);
    EXPECT_EQ(plannedKeys[0], "content:0");
    EXPECT_EQ(plannedKeys[1], "test:1");
    EXPECT_EQ(plannedKeys[2], "test:2");
    EXPECT_EQ(markedKeys[0], "content:0");
}

TEST(TileLoadSchedulerTest, TerrainThenContentShareTerrainContentDispatchBudget) {
    TileLoadLifecycle lifecycle;
    FrameResourceBudgetConfig config;
    config.maxNetworkRequestsPerFrame = 4;
    config.maxTerrainContentNetworkRequestsPerFrame = 1;
    config.maxNetworkInflight = 4;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);

    const TileKey terrainKey{"test", 0, 0, 0};
    const TileKey contentKey{"content", 0, 1, 0};
    const TileKey skippedKey{"content", 0, 2, 0};
    DeferredTerrainProvider terrainProvider;
    CountingContentProvider contentProvider;
    std::vector<std::string> plannedKeys;
    std::vector<std::string> markedKeys;

    const TileLoadRequestOutcome outcome =
        TileLoadScheduler::requestMissingTiles(
            {
                TileLoadRequest{
                    skippedKey,
                    TileLoadPriorityGroup::Preload,
                    10.0},
                TileLoadRequest{
                    contentKey,
                    TileLoadPriorityGroup::Normal,
                    50.0},
                TileLoadRequest{
                    terrainKey,
                    TileLoadPriorityGroup::Urgent,
                    100.0},
            },
            TileLoadSchedulerInput{
                lifecycle,
                budget,
                &contentProvider},
            cacheKeyForTile,
            [&plannedKeys](
                const TileKey& key,
                const std::string&,
                TilesetTile*& tileState) {
                plannedKeys.push_back(
                    key.schemeId.str() + ":" + std::to_string(key.x));
                tileState = nullptr;
                TileLoadRequestSnapshot snapshot;
                if (key.schemeId == "content") {
                    snapshot.contentProviderSupportsTile = true;
                } else {
                }
                return snapshot;
            },
            [](const std::string&) { return false; },
            [](TilesetTile&, double) { return false; },
            [&markedKeys](const TileKey& key) {
                markedKeys.push_back(
                    key.schemeId.str() + ":" + std::to_string(key.x));
            });

    ASSERT_EQ(plannedKeys.size(), 3u);
    ASSERT_EQ(markedKeys.size(), 1u);
    EXPECT_EQ(outcome.issued, 1u);
    EXPECT_FALSE(outcome.blockedByInflight);
    EXPECT_EQ(terrainProvider.requestCount, 0);
    EXPECT_EQ(contentProvider.requestCount, 1);
    EXPECT_EQ(budget.terrainContentNetworkRequestsIssued(), 1u);
    EXPECT_EQ(budget.contentNetworkRequestsIssued(), 1u);
    EXPECT_EQ(budget.networkRequestsIssued(), 1u);
    EXPECT_EQ(plannedKeys[0], "test:0");
    EXPECT_EQ(plannedKeys[1], "content:1");
    EXPECT_EQ(plannedKeys[2], "content:2");
    EXPECT_EQ(markedKeys[0], "content:1");
}
