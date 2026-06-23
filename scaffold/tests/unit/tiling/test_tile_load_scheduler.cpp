#include <gtest/gtest.h>

#include "earth_engine/content/GltfContentProvider.h"
#include "earth_engine/core/resources/FrameResourceBudget.h"
#include "earth_engine/providers/TerrainProvider.h"
#include "earth_engine/tiling/RasterMappedToTilesetTile.h"
#include "earth_engine/tiling/TileLoadLifecycle.h"
#include "earth_engine/tiling/TileLoadScheduler.h"
#include "earth_engine/tiling/TilesetTile.h"

#include <array>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace earth_engine;

namespace {

std::string cacheKeyForTile(const TileKey& key) {
    return key.schemeId + ":" +
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
    TileContentLoadResult decodeContent(const uint8_t*, size_t) override {
        return TileContentLoadResult::failed();
    }

    int requestCount = 0;
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
                snapshot.legacyTerrainProviderSupportsTile = true;
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
                &provider,
                &provider},
            cacheKeyForTile,
            [](const TileKey&,
               const std::string&,
               TilesetTile*& tileState) {
                tileState = nullptr;
                TileLoadRequestSnapshot snapshot;
                snapshot.contentProviderSupportsTile = true;
                snapshot.legacyTerrainProviderSupportsTile = true;
                return snapshot;
            },
            [](const std::string&) { return false; },
            [](TilesetTile&, double) { return false; },
            [&marked](const TileKey&) { marked = true; });

    EXPECT_EQ(outcome.issued, 1u);
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
                nullptr,
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

TEST(TileLoadSchedulerTest, PendingUploadsDoNotConsumeNetworkInflightSlots) {
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
        lifecycle.pendingLoads().addUpload(PendingTileLoad{TileLoadDomain::LegacyHeightmapTerrain,
            firstUploadKey,
            cacheKeyForTile(firstUploadKey),
            TileLoadPriorityGroup::Normal,
            0.0,
            TileLoadResult::createRenderableTerrain()});
        lifecycle.pendingLoads().addUpload(PendingTileLoad{TileLoadDomain::LegacyHeightmapTerrain,
            secondUploadKey,
            cacheKeyForTile(secondUploadKey),
            TileLoadPriorityGroup::Normal,
            0.0,
            TileLoadResult::createRenderableTerrain()});
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
                &provider,
                nullptr},
            cacheKeyForTile,
            [](const TileKey&,
               const std::string&,
               TilesetTile*& tileState) {
                tileState = nullptr;
                TileLoadRequestSnapshot snapshot;
                snapshot.legacyTerrainProviderSupportsTile = true;
                return snapshot;
            },
            [](const std::string&) { return false; },
            [](TilesetTile&, double) { return false; },
            [&marked](const TileKey&) { marked = true; });

    EXPECT_EQ(outcome.issued, 1u);
    EXPECT_FALSE(outcome.blockedByInflight);
    EXPECT_EQ(provider.requestCount, 1);
    EXPECT_TRUE(marked);
    EXPECT_EQ(lifecycle.counts().terrainUploads, 2u);
    EXPECT_EQ(lifecycle.pendingRequestCount(), 1u);
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
    child.content.upsampledFromParent = true;
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
                &legacyProvider,
                &provider},
            cacheKeyForTile,
            [&child](
                const TileKey&,
                const std::string&,
                TilesetTile*& tileState) {
                tileState = &child;
                TileLoadRequestSnapshot snapshot;
                snapshot.hasTile = true;
                snapshot.upsampledFromParent = true;
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
    EXPECT_EQ(lifecycle.counts().terrainUploads, 0u);
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
    child.content.upsampledFromParent = true;
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
                &legacyProvider,
                &provider},
            cacheKeyForTile,
            [&child](
                const TileKey&,
                const std::string&,
                TilesetTile*& tileState) {
                tileState = &child;
                TileLoadRequestSnapshot snapshot;
                snapshot.hasTile = true;
                snapshot.upsampledFromParent = true;
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
    EXPECT_FALSE(outcome.blockedByInflight);
    EXPECT_TRUE(prepared);
    EXPECT_TRUE(marked);
    EXPECT_EQ(provider.requestCount, 0);
    EXPECT_EQ(legacyProvider.requestCount, 0);
    EXPECT_EQ(lifecycle.counts().terrainUploads, 1u);
    EXPECT_EQ(lifecycle.counts().contentUploads, 0u);

    PendingLoadFinalizeContext finalizeContext{false, budget};
    std::optional<PendingTileLoad> pending;
    {
        std::lock_guard<std::mutex> lock(lifecycle.mutex());
        pending =
            lifecycle.pendingLoads().takeHighestPriorityUpload(
                finalizeContext);
    }
    ASSERT_TRUE(pending.has_value());
    EXPECT_EQ(pending->domain, TileLoadDomain::GltfTerrain);
    EXPECT_EQ(pending->result.status, TileLoadStatus::Renderable);
    EXPECT_TRUE(pending->content().hasGltfTerrainPayload());
    ASSERT_NE(pending->content().gltfModel, nullptr);
    EXPECT_FALSE(pending->content().heightmap);
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
                snapshot.legacyTerrainProviderSupportsTile = true;
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

TEST(TileLoadSchedulerTest, SortsAndQueuesUpsampledTerrain) {
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
    normalTile.content.upsampledFromParent = true;
    urgentTile.content.upsampledFromParent = true;
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
                nullptr,
                nullptr},
            cacheKeyForTile,
            [&urgentKey, &normalTile, &urgentTile](
                const TileKey& key,
                const std::string&,
                TilesetTile*& tileState) {
                tileState = key == urgentKey ? &urgentTile : &normalTile;
                TileLoadRequestSnapshot snapshot;
                snapshot.hasTile = true;
                snapshot.upsampledFromParent = true;
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

    ASSERT_EQ(prepareOrder.size(), 2u);
    EXPECT_EQ(outcome.issued, 2u);
    EXPECT_FALSE(outcome.blockedByInflight);
    EXPECT_EQ(prepareOrder[0], urgentKey.x);
    EXPECT_EQ(prepareOrder[1], normalKey.x);
    EXPECT_EQ(markedOrder, prepareOrder);
    EXPECT_EQ(lifecycle.counts().terrainUploads, 2u);
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
    waitingTile.content.upsampledFromParent = true;
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
                &provider,
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
                    snapshot.upsampledFromParent = true;
                } else {
                    tileState = nullptr;
                    snapshot.legacyTerrainProviderSupportsTile = true;
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
    ASSERT_EQ(markedLevels.size(), 1u);
    EXPECT_EQ(outcome.issued, 1u);
    EXPECT_FALSE(outcome.blockedByInflight);
    EXPECT_EQ(plannedLevels[0], waitingUpsampleKey.z);
    EXPECT_EQ(plannedLevels[1], loadableTerrainKey.z);
    EXPECT_EQ(preparedLevels.front(), waitingUpsampleKey.z);
    EXPECT_EQ(provider.requestCount, 1);
    EXPECT_EQ(markedLevels.front(), loadableTerrainKey.z);
    EXPECT_EQ(lifecycle.pendingRequestCount(), 1u);
    EXPECT_EQ(lifecycle.counts().terrainUploads, 0u);
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
    readyTile.content.upsampledFromParent = true;
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
                nullptr,
                nullptr},
            cacheKeyForTile,
            [&missingTileStateKey, &readyTile, &plannedColumns](
                const TileKey& key,
                const std::string&,
                TilesetTile*& tileState) {
                plannedColumns.push_back(key.x);
                TileLoadRequestSnapshot snapshot;
                snapshot.hasTile = true;
                snapshot.upsampledFromParent = true;
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
    ASSERT_EQ(markedColumns.size(), 1u);
    EXPECT_EQ(outcome.issued, 1u);
    EXPECT_FALSE(outcome.blockedByInflight);
    EXPECT_EQ(plannedColumns[0], missingTileStateKey.x);
    EXPECT_EQ(plannedColumns[1], readyUpsampleKey.x);
    EXPECT_EQ(preparedColumns.front(), readyUpsampleKey.x);
    EXPECT_EQ(markedColumns.front(), readyUpsampleKey.x);
    EXPECT_EQ(lifecycle.counts().terrainUploads, 1u);
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
    tile.content.upsampledFromParent = true;
    const std::string cacheKey = cacheKeyForTile(key);
    {
        std::lock_guard<std::mutex> lock(lifecycle.mutex());
        lifecycle.pendingLoads().addTerminalResult(PendingTileLoad{TileLoadDomain::LegacyHeightmapTerrain,
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

    EXPECT_EQ(outcome.issued, 0u);
    EXPECT_FALSE(outcome.blockedByInflight);
    EXPECT_FALSE(prepared);
    EXPECT_FALSE(marked);
    EXPECT_EQ(lifecycle.counts().terrainTerminalResults, 1u);
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
        lifecycle.pendingLoads().addUpload(PendingTileLoad{TileLoadDomain::LegacyHeightmapTerrain,
            key,
            cacheKey,
            TileLoadPriorityGroup::Normal,
            0.0,
            TileLoadResult::createRenderableTerrain()});
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
        lifecycle.pendingLoads().addUpload(PendingTileLoad{TileLoadDomain::LegacyHeightmapTerrain,
            key,
            cacheKey,
            TileLoadPriorityGroup::Normal,
            0.0,
            TileLoadResult::createRenderableTerrain()});
        ASSERT_TRUE(lifecycle.pendingLoads()
                        .takeHighestPriorityUpload(false, budget)
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
                nullptr,
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
                nullptr,
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
                &provider,
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
                    lifecycle.pendingLoads().addTerminalResult(PendingTileLoad{TileLoadDomain::LegacyHeightmapTerrain,
                            key,
                            cacheKey,
                            TileLoadPriorityGroup::Normal,
                            0.0,
                            TileLoadStatus::RetryLater});
                }
                TileLoadRequestSnapshot snapshot;
                snapshot.legacyTerrainProviderSupportsTile = true;
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
    EXPECT_EQ(lifecycle.counts().terrainTerminalResults, 1u);
}

TEST(TileLoadSchedulerTest, StopsAfterDispatchBudgetBlock) {
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
                nullptr,
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

    ASSERT_EQ(plannedKeys.size(), 2u);
    ASSERT_EQ(markedKeys.size(), 1u);
    EXPECT_EQ(outcome.issued, 1u);
    EXPECT_FALSE(outcome.blockedByInflight);
    EXPECT_EQ(provider.requestCount, 1);
    EXPECT_EQ(plannedKeys[0], firstKey.x);
    EXPECT_EQ(plannedKeys[1], blockedKey.x);
    EXPECT_EQ(markedKeys[0], firstKey.x);
}

TEST(TileLoadSchedulerTest, StopsAfterTerrainDispatchBudgetBlock) {
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
                &provider,
                nullptr},
            cacheKeyForTile,
            [&plannedKeys](
                const TileKey& key,
                const std::string&,
                TilesetTile*& tileState) {
                plannedKeys.push_back(key.x);
                tileState = nullptr;
                TileLoadRequestSnapshot snapshot;
                snapshot.legacyTerrainProviderSupportsTile = true;
                return snapshot;
            },
            [](const std::string&) { return false; },
            [](TilesetTile&, double) { return false; },
            [&markedKeys](const TileKey& key) {
                markedKeys.push_back(key.x);
            });

    ASSERT_EQ(plannedKeys.size(), 2u);
    ASSERT_EQ(markedKeys.size(), 1u);
    EXPECT_EQ(outcome.issued, 1u);
    EXPECT_FALSE(outcome.blockedByInflight);
    EXPECT_EQ(provider.requestCount, 1);
    EXPECT_EQ(plannedKeys[0], firstKey.x);
    EXPECT_EQ(plannedKeys[1], blockedKey.x);
    EXPECT_EQ(markedKeys[0], firstKey.x);
}

TEST(TileLoadSchedulerTest, ContentThenTerrainShareDispatchBudget) {
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
                &terrainProvider,
                &contentProvider},
            cacheKeyForTile,
            [&plannedKeys](
                const TileKey& key,
                const std::string&,
                TilesetTile*& tileState) {
                plannedKeys.push_back(
                    key.schemeId + ":" + std::to_string(key.x));
                tileState = nullptr;
                TileLoadRequestSnapshot snapshot;
                if (key.schemeId == "content") {
                    snapshot.contentProviderSupportsTile = true;
                } else {
                    snapshot.legacyTerrainProviderSupportsTile = true;
                }
                return snapshot;
            },
            [](const std::string&) { return false; },
            [](TilesetTile&, double) { return false; },
            [&markedKeys](const TileKey& key) {
                markedKeys.push_back(
                    key.schemeId + ":" + std::to_string(key.x));
            });

    ASSERT_EQ(plannedKeys.size(), 2u);
    ASSERT_EQ(markedKeys.size(), 1u);
    EXPECT_EQ(outcome.issued, 1u);
    EXPECT_FALSE(outcome.blockedByInflight);
    EXPECT_EQ(contentProvider.requestCount, 1);
    EXPECT_EQ(terrainProvider.requestCount, 0);
    EXPECT_EQ(budget.terrainContentNetworkRequestsIssued(), 1u);
    EXPECT_EQ(budget.networkRequestsIssued(), 1u);
    EXPECT_EQ(plannedKeys[0], "content:0");
    EXPECT_EQ(plannedKeys[1], "test:1");
    EXPECT_EQ(markedKeys[0], "content:0");
}

TEST(TileLoadSchedulerTest, TerrainThenContentShareDispatchBudget) {
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
                &terrainProvider,
                &contentProvider},
            cacheKeyForTile,
            [&plannedKeys](
                const TileKey& key,
                const std::string&,
                TilesetTile*& tileState) {
                plannedKeys.push_back(
                    key.schemeId + ":" + std::to_string(key.x));
                tileState = nullptr;
                TileLoadRequestSnapshot snapshot;
                if (key.schemeId == "content") {
                    snapshot.contentProviderSupportsTile = true;
                } else {
                    snapshot.legacyTerrainProviderSupportsTile = true;
                }
                return snapshot;
            },
            [](const std::string&) { return false; },
            [](TilesetTile&, double) { return false; },
            [&markedKeys](const TileKey& key) {
                markedKeys.push_back(
                    key.schemeId + ":" + std::to_string(key.x));
            });

    ASSERT_EQ(plannedKeys.size(), 2u);
    ASSERT_EQ(markedKeys.size(), 1u);
    EXPECT_EQ(outcome.issued, 1u);
    EXPECT_FALSE(outcome.blockedByInflight);
    EXPECT_EQ(terrainProvider.requestCount, 1);
    EXPECT_EQ(contentProvider.requestCount, 0);
    EXPECT_EQ(budget.terrainContentNetworkRequestsIssued(), 1u);
    EXPECT_EQ(budget.networkRequestsIssued(), 1u);
    EXPECT_EQ(plannedKeys[0], "test:0");
    EXPECT_EQ(plannedKeys[1], "content:1");
    EXPECT_EQ(markedKeys[0], "test:0");
}
