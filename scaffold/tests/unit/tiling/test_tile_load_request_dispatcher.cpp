#include <gtest/gtest.h>

#include "earth_engine/content/GltfContentProvider.h"
#include "earth_engine/providers/TerrainProvider.h"
#include "earth_engine/core/math/Vec3.h"
#include "earth_engine/tiling/TileBoundingVolume.h"
#include "earth_engine/tiling/TileLoadDomainPolicy.h"
#include "earth_engine/tiling/TileLoadLifecycle.h"
#include "earth_engine/tiling/TileLoadRequestDispatcher.h"

#include <condition_variable>
#include <mutex>

using namespace earth_engine;

namespace {

TileLoadResult makeMalformedRenderableWithoutPayloadForTest() {
    TileLoadResult result;
    result.status = TileLoadStatus::Renderable;
    return result;
}

std::unique_ptr<GltfModel> makeTerrainGltfModelForTest(
    const Rectangle& rectangle =
        Rectangle::fromDegrees(-1.0, -1.0, 1.0, 1.0)) {
    auto model = std::make_unique<GltfModel>();
    model->rasterOverlayDetails.setGeographicRectangle(rectangle);
    return model;
}

} // namespace

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
        TerrainCallback,
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

class FanoutTerrainProvider final : public TerrainProvider {
public:
    std::string id() const override { return "fanout-terrain"; }
    std::string schemeId() const override { return "test"; }
    int minZoom() const override { return 0; }
    int maxZoom() const override { return 1; }
    int tileSize() const override { return 2; }
    int estimatedRequestFanout(const TileKey&) const override { return 2; }
    std::string buildUrl(const TileKey&) const override {
        return "memory://fanout-terrain";
    }
    void requestTile(
        const TileKey&,
        CancellationToken,
        TerrainCallback,
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

class SyncTerminalTerrainProvider final : public TerrainProvider {
public:
    explicit SyncTerminalTerrainProvider(bool& issuedBeforeCallback)
        : issuedBeforeCallback_(issuedBeforeCallback) {}

    std::string id() const override { return "dispatcher-terrain-terminal"; }
    std::string schemeId() const override { return "test"; }
    int minZoom() const override { return 0; }
    int maxZoom() const override { return 1; }
    int tileSize() const override { return 2; }
    std::string buildUrl(const TileKey&) const override {
        return "memory://dispatcher-terrain-terminal";
    }
    void requestTile(
        const TileKey& key,
        CancellationToken,
        TerrainCallback callback,
        HttpRequestPriority = HttpRequestPriority::Normal) override {
        callbackSawIssued = issuedBeforeCallback_;
        callback(key, TerrainTileLoadResult::retryLater());
    }
    std::unique_ptr<DecodedHeightmap> decodeTile(
        const uint8_t*,
        size_t) override {
        return nullptr;
    }

    bool& issuedBeforeCallback_;
    bool callbackSawIssued = false;
};

class SyncEmptyTerrainProvider final : public TerrainProvider {
public:
    std::string id() const override {
        return "dispatcher-empty-terrain";
    }
    std::string schemeId() const override { return "test"; }
    int minZoom() const override { return 0; }
    int maxZoom() const override { return 1; }
    int tileSize() const override { return 2; }
    std::string buildUrl(const TileKey&) const override {
        return "memory://dispatcher-empty-terrain";
    }
    void requestTile(
        const TileKey& key,
        CancellationToken,
        TerrainCallback callback,
        HttpRequestPriority = HttpRequestPriority::Normal) override {
        callback(key, TerrainTileLoadResult::empty());
    }
    std::unique_ptr<DecodedHeightmap> decodeTile(
        const uint8_t*,
        size_t) override {
        return nullptr;
    }
};

class SyncUploadTerrainProvider final : public TerrainProvider {
public:
    explicit SyncUploadTerrainProvider(bool& issuedBeforeCallback)
        : issuedBeforeCallback_(issuedBeforeCallback) {}

    std::string id() const override { return "dispatcher-terrain-upload"; }
    std::string schemeId() const override { return "test"; }
    int minZoom() const override { return 0; }
    int maxZoom() const override { return 1; }
    int tileSize() const override { return 2; }
    std::string buildUrl(const TileKey&) const override {
        return "memory://dispatcher-terrain-upload";
    }
    void requestTile(
        const TileKey& key,
        CancellationToken,
        TerrainCallback callback,
        HttpRequestPriority = HttpRequestPriority::Normal) override {
        callbackSawIssued = issuedBeforeCallback_;
        auto heightmap = std::make_unique<DecodedHeightmap>();
        heightmap->tileSize = 2;
        heightmap->heights = {0.0f, 0.0f, 0.0f, 0.0f};
        callback(key, TerrainTileLoadResult::successWithHeightmap(std::move(heightmap)));
    }
    std::unique_ptr<DecodedHeightmap> decodeTile(
        const uint8_t*,
        size_t) override {
        return nullptr;
    }

    bool& issuedBeforeCallback_;
    bool callbackSawIssued = false;
};

class DeferredTerrainProvider final : public TerrainProvider {
public:
    std::string id() const override { return "dispatcher-deferred-terrain"; }
    std::string schemeId() const override { return "test"; }
    int minZoom() const override { return 0; }
    int maxZoom() const override { return 1; }
    int tileSize() const override { return 2; }
    std::string buildUrl(const TileKey&) const override {
        return "memory://dispatcher-deferred-terrain";
    }
    void requestTile(
        const TileKey&,
        CancellationToken,
        TerrainCallback callback,
        HttpRequestPriority = HttpRequestPriority::Normal) override {
        terrainCallback = std::move(callback);
    }
    std::unique_ptr<DecodedHeightmap> decodeTile(
        const uint8_t*,
        size_t) override {
        return nullptr;
    }

    TerrainCallback terrainCallback;
};

class RecordingPriorityTerrainProvider final : public TerrainProvider {
public:
    std::string id() const override { return "dispatcher-priority-terrain"; }
    std::string schemeId() const override { return "test"; }
    int minZoom() const override { return 0; }
    int maxZoom() const override { return 1; }
    int tileSize() const override { return 2; }
    std::string buildUrl(const TileKey&) const override {
        return "memory://dispatcher-priority-terrain";
    }
    void requestTile(
        const TileKey& key,
        CancellationToken,
        TerrainCallback callback,
        HttpRequestPriority priority = HttpRequestPriority::Normal) override {
        observedPriority = priority;
        callback(key, TerrainTileLoadResult::retryLater());
    }
    std::unique_ptr<DecodedHeightmap> decodeTile(
        const uint8_t*,
        size_t) override {
        return nullptr;
    }

    HttpRequestPriority observedPriority = HttpRequestPriority::Normal;
};

class SyncTerminalContentProvider final : public TilesetContentProvider {
public:
    explicit SyncTerminalContentProvider(bool& issuedBeforeCallback)
        : issuedBeforeCallback_(issuedBeforeCallback) {}

    std::string id() const override { return "dispatcher-content-terminal"; }
    bool supportsTile(const TileKey&) const override { return true; }
    void requestTileContent(
        const TileKey& key,
        CancellationToken,
        ContentCallback callback,
        HttpRequestPriority = HttpRequestPriority::Normal) override {
        callbackSawIssued = issuedBeforeCallback_;
        callback(key, TileContentLoadResult::empty());
    }
    TileContentLoadResult decodeContent(const uint8_t*, size_t) override {
        return TileContentLoadResult::failed();
    }

    bool& issuedBeforeCallback_;
    bool callbackSawIssued = false;
};

class SyncTerminalMetadataContentProvider final : public TilesetContentProvider {
public:
    std::string id() const override {
        return "dispatcher-content-terminal-metadata";
    }
    bool supportsTile(const TileKey&) const override { return true; }
    void requestTileContent(
        const TileKey& key,
        CancellationToken,
        ContentCallback callback,
        HttpRequestPriority = HttpRequestPriority::Normal) override {
        TileContentLoadResult result = TileContentLoadResult::external();
        result.metadata.updatedBoundingVolume =
            TileBoundingVolume::fromRegion(
                Rectangle(0.2, 0.3, 0.4, 0.5),
                5.0,
                50.0);
        callback(key, std::move(result));
    }
    TileContentLoadResult decodeContent(const uint8_t*, size_t) override {
        return TileContentLoadResult::failed();
    }
};

class DeferredContentProvider final : public TilesetContentProvider {
public:
    std::string id() const override { return "dispatcher-deferred-content"; }
    bool supportsTile(const TileKey&) const override { return true; }
    void requestTileContent(
        const TileKey&,
        CancellationToken,
        ContentCallback callback,
        HttpRequestPriority = HttpRequestPriority::Normal) override {
        contentCallback = std::move(callback);
    }
    TileContentLoadResult decodeContent(const uint8_t*, size_t) override {
        return TileContentLoadResult::failed();
    }

    ContentCallback contentCallback;
};

class RecordingPriorityContentProvider final : public TilesetContentProvider {
public:
    std::string id() const override { return "dispatcher-priority-content"; }
    bool supportsTile(const TileKey&) const override { return true; }
    void requestTileContent(
        const TileKey& key,
        CancellationToken,
        ContentCallback callback,
        HttpRequestPriority priority = HttpRequestPriority::Normal) override {
        observedPriority = priority;
        callback(key, TileContentLoadResult::empty());
    }
    TileContentLoadResult decodeContent(const uint8_t*, size_t) override {
        return TileContentLoadResult::failed();
    }

    HttpRequestPriority observedPriority = HttpRequestPriority::Normal;
};

class SyncRenderContentProvider final : public TilesetContentProvider {
public:
    explicit SyncRenderContentProvider(bool& issuedBeforeCallback)
        : issuedBeforeCallback_(issuedBeforeCallback) {}

    std::string id() const override { return "dispatcher-content-render"; }
    bool supportsTile(const TileKey&) const override { return true; }
    void requestTileContent(
        const TileKey& key,
        CancellationToken,
        ContentCallback callback,
        HttpRequestPriority = HttpRequestPriority::Normal) override {
        callbackSawIssued = issuedBeforeCallback_;
        callback(
            key,
            TileContentLoadResult::render(std::make_unique<GltfModel>()));
    }
    TileContentLoadResult decodeContent(const uint8_t*, size_t) override {
        return TileContentLoadResult::failed();
    }

    bool& issuedBeforeCallback_;
    bool callbackSawIssued = false;
};

class SyncRenderTerrainContentProvider final : public TilesetContentProvider {
public:
    explicit SyncRenderTerrainContentProvider(bool& issuedBeforeCallback)
        : issuedBeforeCallback_(issuedBeforeCallback) {}

    std::string id() const override {
        return "dispatcher-gltf-terrain-render";
    }
    bool supportsTile(const TileKey&) const override { return true; }
    bool providesTerrainQuadtree() const override { return true; }
    void requestTileContent(
        const TileKey& key,
        CancellationToken,
        ContentCallback callback,
        HttpRequestPriority = HttpRequestPriority::Normal) override {
        callbackSawIssued = issuedBeforeCallback_;
        TileContentLoadResult result =
            TileContentLoadResult::renderTerrain(
                makeTerrainGltfModelForTest());
        callback(key, std::move(result));
    }
    TileContentLoadResult decodeContent(const uint8_t*, size_t) override {
        return TileContentLoadResult::failed();
    }

    bool& issuedBeforeCallback_;
    bool callbackSawIssued = false;
};

class SyncOrdinaryRenderFromTerrainQuadtreeProvider final
    : public TilesetContentProvider {
public:
    explicit SyncOrdinaryRenderFromTerrainQuadtreeProvider(
        bool& issuedBeforeCallback)
        : issuedBeforeCallback_(issuedBeforeCallback) {}

    std::string id() const override {
        return "dispatcher-terrain-ordinary-render";
    }
    bool supportsTile(const TileKey&) const override { return true; }
    bool providesTerrainQuadtree() const override { return true; }
    void requestTileContent(
        const TileKey& key,
        CancellationToken,
        ContentCallback callback,
        HttpRequestPriority = HttpRequestPriority::Normal) override {
        callbackSawIssued = issuedBeforeCallback_;
        callback(
            key,
            TileContentLoadResult::render(std::make_unique<GltfModel>()));
    }
    TileContentLoadResult decodeContent(const uint8_t*, size_t) override {
        return TileContentLoadResult::failed();
    }

    bool& issuedBeforeCallback_;
    bool callbackSawIssued = false;
};

TEST(TileLoadRequestDispatcherTest,
     RunsOnIssuedBeforeSynchronousContentTerminalCallback) {
    std::mutex mutex;
    std::condition_variable condition;
    TilePendingRequestState requestState;
    TilePendingLoadQueue pendingLoads;
    FrameResourceBudgetConfig config;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);
    const TileKey key{"test", 0, 0, 0};
    bool issued = false;
    SyncTerminalContentProvider provider(issued);

    TileLoadDispatchResult result =
        TileLoadRequestDispatcher::requestContent(
            mutex,
            condition,
            requestState,
            pendingLoads,
            budget,
            provider,
            key,
            "content",
            TileLoadPriorityGroup::Normal,
            0.0,
            [&issued]() { issued = true; });

    EXPECT_EQ(TileLoadDispatchResult::Issued, result);
    EXPECT_TRUE(provider.callbackSawIssued);
    EXPECT_TRUE(requestState.empty());
    EXPECT_EQ(1u, pendingLoads.contentTerminalResultCount());
}

TEST(TileLoadRequestDispatcherTest,
     ContentTerminalResultKeepsTileLoadResultMetadata) {
    std::mutex mutex;
    std::condition_variable condition;
    TilePendingRequestState requestState;
    TilePendingLoadQueue pendingLoads;
    FrameResourceBudgetConfig config;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);
    const TileKey key{"test", 0, 0, 0};
    SyncTerminalMetadataContentProvider provider;

    TileLoadDispatchResult result =
        TileLoadRequestDispatcher::requestContent(
            mutex,
            condition,
            requestState,
            pendingLoads,
            budget,
            provider,
            key,
            "content-terminal-metadata",
            TileLoadPriorityGroup::Normal,
            0.0,
            []() {});

    EXPECT_EQ(TileLoadDispatchResult::Issued, result);
    EXPECT_TRUE(requestState.empty());
    ASSERT_EQ(1u, pendingLoads.contentTerminalResultCount());

    auto pending = pendingLoads.takeHighestPriorityTerminalResult(budget);
    ASSERT_TRUE(pending.has_value());
    EXPECT_EQ(TileLoadDomain::Content, pending->domain);
    EXPECT_EQ(TileLoadStatus::External, pending->result.status);
    ASSERT_TRUE(
        pending->content().metadata.updatedBoundingVolume.has_value());
    EXPECT_DOUBLE_EQ(
        5.0,
        pending->content().metadata.updatedBoundingVolume->minimumHeight);
    EXPECT_DOUBLE_EQ(
        50.0,
        pending->content().metadata.updatedBoundingVolume->maximumHeight);
}

TEST(TileLoadRequestDispatcherTest,
     TerrainContentUsesGltfContentResult) {
    auto gltfModel = makeTerrainGltfModelForTest();
    GltfModel* rawGltfModel = gltfModel.get();
    TileContentLoadResult contentResult =
        TileContentLoadResult::renderTerrain(std::move(gltfModel));
    TileLoadResult normalizedGltf =
        TileLoadResult::fromContentResult(std::move(contentResult));

    EXPECT_EQ(TileLoadStatus::Renderable, normalizedGltf.status);
    EXPECT_TRUE(normalizedGltf.shouldUpload());
    EXPECT_TRUE(normalizedGltf.content.terrainRenderContent);
    EXPECT_TRUE(normalizedGltf.isRenderableContentTerrain());
    EXPECT_EQ(rawGltfModel, normalizedGltf.content.gltfModel.get());

    TileContentLoadResult failedContentResult =
        TileContentLoadResult::failed();
    TileLoadResult normalizedFailed =
        TileLoadResult::fromContentResult(std::move(failedContentResult));
    EXPECT_EQ(TileLoadStatus::Failed, normalizedFailed.status);

    TileLoadResult terrainWithoutRasterDetails =
        TileLoadResult::fromContentResult(
            TileContentLoadResult::renderTerrain(
                std::make_unique<GltfModel>()));
    EXPECT_EQ(TileLoadStatus::Renderable,
              terrainWithoutRasterDetails.status);
    EXPECT_TRUE(terrainWithoutRasterDetails.shouldUpload());
    EXPECT_TRUE(terrainWithoutRasterDetails.isRenderableContentTerrain());
    EXPECT_FALSE(
        terrainWithoutRasterDetails.content.metadata.rasterOverlayDetails
            .has_value());
    TileLoadResult normalizedTerrainWithoutRasterDetails =
        TileLoadDomainPolicy::normalizeForDomain(
            TileLoadDomain::TerrainContent,
            std::move(terrainWithoutRasterDetails));
    EXPECT_EQ(TileLoadStatus::Renderable,
              normalizedTerrainWithoutRasterDetails.status);
    EXPECT_TRUE(normalizedTerrainWithoutRasterDetails.shouldUpload());

    auto mismatchedModel = makeTerrainGltfModelForTest(
        Rectangle::fromDegrees(1.0, 2.0, 3.0, 4.0));
    TileLoadResultMetadata mismatchedMetadata;
    mismatchedMetadata.rasterOverlayDetails.emplace();
    mismatchedMetadata.rasterOverlayDetails->setGeographicRectangle(
        Rectangle::fromDegrees(5.0, 6.0, 7.0, 8.0));
    TileLoadedContent mismatchedContent =
        TileLoadedContent::fromContentResult(
            TileContentLoadResult::renderTerrain(
                std::move(mismatchedModel),
                mismatchedMetadata));
    mismatchedContent.gltfModel->rasterOverlayDetails.setGeographicRectangle(
        Rectangle::fromDegrees(1.0, 2.0, 3.0, 4.0));
    EXPECT_FALSE(mismatchedContent.satisfiesContentTerrainPayloadContract());

    TileLoadResult renderableWithoutPayload =
        makeMalformedRenderableWithoutPayloadForTest();
    EXPECT_FALSE(renderableWithoutPayload.shouldUpload());

    TileContentLoadResult malformedContentResult;
    malformedContentResult.status = TileLoadStatus::Renderable;
    malformedContentResult.metadata.updatedBoundingVolume =
        TileBoundingVolume::fromRegion(
            Rectangle::fromDegrees(-1.0, -2.0, 3.0, 4.0),
            -5.0,
            6.0);
    TileLoadResult normalizedMalformed =
        TileLoadResult::fromContentResult(std::move(malformedContentResult));
    EXPECT_EQ(TileLoadStatus::Failed, normalizedMalformed.status);
    EXPECT_FALSE(normalizedMalformed.shouldUpload());
    EXPECT_FALSE(
        normalizedMalformed.content.metadata.updatedBoundingVolume.has_value());

    auto directGltfModel = std::make_unique<GltfModel>();
    GltfModel* rawDirectGltfModel = directGltfModel.get();
    const Rectangle modelRasterRectangle =
        Rectangle::fromDegrees(10.0, 11.0, 12.0, 13.0);
    directGltfModel->rasterOverlayDetails.setGeographicRectangle(
        modelRasterRectangle,
        -3.0,
        4.0);
    TileLoadResultMetadata directMetadata;
    directMetadata.updatedBoundingVolume =
        TileBoundingVolume::fromRegion(
            Rectangle::fromDegrees(1.0, 2.0, 3.0, 4.0),
            -10.0,
            20.0);
    const Mat4 directTransform = Mat4::translation(Vec3{1.0, 2.0, 3.0});
    TileLoadResult directTerrainContent =
        TileLoadResult::createRenderableGltfTerrain(
            std::move(directGltfModel),
            directMetadata,
            directTransform);
    EXPECT_TRUE(directTerrainContent.shouldUpload());
    EXPECT_TRUE(directTerrainContent.content.hasGltfTerrainPayload());
    EXPECT_EQ(rawDirectGltfModel, directTerrainContent.content.gltfModel.get());
    EXPECT_EQ(directTransform, directTerrainContent.content.contentTransform);
    ASSERT_TRUE(
        directTerrainContent.content.metadata.updatedBoundingVolume.has_value());
    const TileBoundingVolume& committedVolume =
        *directTerrainContent.content.metadata.updatedBoundingVolume;
    EXPECT_EQ(TileBoundingVolumeKind::Region, committedVolume.kind);
    EXPECT_EQ(directMetadata.updatedBoundingVolume->region,
              committedVolume.region);
    EXPECT_DOUBLE_EQ(-10.0, committedVolume.minimumHeight);
    EXPECT_DOUBLE_EQ(20.0, committedVolume.maximumHeight);
    ASSERT_TRUE(
        directTerrainContent.content.metadata.rasterOverlayDetails.has_value());
    const Rectangle* inheritedRasterRectangle =
        directTerrainContent.content.metadata.rasterOverlayDetails
            ->findRectangleForOverlayProjection(
                RasterOverlayProjection::Geographic);
    ASSERT_NE(nullptr, inheritedRasterRectangle);
    EXPECT_EQ(modelRasterRectangle, *inheritedRasterRectangle);

    auto explicitModel = std::make_unique<GltfModel>();
    explicitModel->rasterOverlayDetails.setGeographicRectangle(
        Rectangle::fromDegrees(-40.0, -30.0, -20.0, -10.0));
    TileLoadResultMetadata explicitMetadata;
    const Rectangle explicitRasterRectangle =
        Rectangle::fromDegrees(30.0, 31.0, 32.0, 33.0);
    explicitMetadata.rasterOverlayDetails.emplace();
    explicitMetadata.rasterOverlayDetails->setGeographicRectangle(
        explicitRasterRectangle);
    TileLoadResult explicitTerrainContent =
        TileLoadResult::createRenderableGltfTerrain(
            std::move(explicitModel),
            explicitMetadata);
    ASSERT_TRUE(
        explicitTerrainContent.content.metadata.rasterOverlayDetails.has_value());
    const Rectangle* explicitCommittedRectangle =
        explicitTerrainContent.content.metadata.rasterOverlayDetails
            ->findRectangleForOverlayProjection(
                RasterOverlayProjection::Geographic);
    ASSERT_NE(nullptr, explicitCommittedRectangle);
    EXPECT_EQ(explicitRasterRectangle, *explicitCommittedRectangle);
    ASSERT_NE(nullptr, explicitTerrainContent.content.gltfModel);
    const Rectangle* explicitModelRectangle =
        explicitTerrainContent.content.gltfModel->rasterOverlayDetails
            .findRectangleForOverlayProjection(
                RasterOverlayProjection::Geographic);
    ASSERT_NE(nullptr, explicitModelRectangle);
    EXPECT_EQ(explicitRasterRectangle, *explicitModelRectangle);

    TileLoadResult contentGltf = TileLoadResult::fromContentResult(
        TileContentLoadResult::render(std::make_unique<GltfModel>()));
    EXPECT_TRUE(contentGltf.shouldUpload());
    TileLoadResult normalizedContentGltfForTerrain =
        TileLoadDomainPolicy::normalizeForDomain(
            TileLoadDomain::TerrainContent,
            std::move(contentGltf));
    EXPECT_EQ(
        TileLoadStatus::Failed,
        normalizedContentGltfForTerrain.status);
    EXPECT_FALSE(normalizedContentGltfForTerrain.shouldUpload());
    EXPECT_FALSE(normalizedContentGltfForTerrain.isRenderableContentTerrain());

    std::mutex queueMutex;
    TilePendingRequestState requestState;
    TilePendingLoadQueue pendingLoads;
    TileLoadDispatchResult queuedOrdinaryGltfAsTerrain =
        TileLoadRequestDispatcher::queueUpsampledLoad(
            queueMutex,
            requestState,
            pendingLoads,
            TileKey{"Geographic-TMS", 1, 0, 0},
            "ordinary-gltf-as-terrain",
            TileLoadPriorityGroup::Normal,
            0.0,
            TileLoadDomain::TerrainContent,
            TileLoadResult::fromContentResult(
                TileContentLoadResult::render(std::make_unique<GltfModel>())));
    EXPECT_EQ(TileLoadDispatchResult::Issued, queuedOrdinaryGltfAsTerrain);
    ASSERT_EQ(1u, pendingLoads.gltfTerrainTerminalResultCount());
    FrameResourceBudgetConfig ordinaryGltfConfig;
    FrameResourceBudget ordinaryGltfBudget;
    ordinaryGltfBudget.beginFrame(1, ordinaryGltfConfig);
    std::optional<PendingTileLoad> ordinaryGltfTerminal =
        pendingLoads.takeHighestPriorityTerminalResult(ordinaryGltfBudget);
    ASSERT_TRUE(ordinaryGltfTerminal.has_value());
    EXPECT_EQ(TileLoadDomain::TerrainContent, ordinaryGltfTerminal->domain);
    EXPECT_EQ(TileLoadStatus::Failed, ordinaryGltfTerminal->result.status);
    EXPECT_FALSE(ordinaryGltfTerminal->content().hasGltfTerrainPayload());

    auto contentModel = std::make_unique<GltfModel>();
    GltfModel* rawContentModel = contentModel.get();
    const Rectangle contentRasterRectangle =
        Rectangle::fromDegrees(50.0, 51.0, 52.0, 53.0);
    contentModel->rasterOverlayDetails.setGeographicRectangle(
        contentRasterRectangle);
    const Mat4 contentTransform = Mat4::translation(Vec3{4.0, 5.0, 6.0});
    TileContentLoadResult terrainContentResult =
        TileContentLoadResult::renderTerrain(
            std::move(contentModel),
            {},
            contentTransform);
    EXPECT_EQ(TileLoadStatus::Renderable, terrainContentResult.status);
    EXPECT_TRUE(terrainContentResult.terrainRenderContent);
    EXPECT_EQ(rawContentModel, terrainContentResult.gltfModel.get());
    EXPECT_EQ(contentTransform, terrainContentResult.contentTransform);
    ASSERT_TRUE(terrainContentResult.metadata.rasterOverlayDetails.has_value());
    const Rectangle* contentCommittedRectangle =
        terrainContentResult.metadata.rasterOverlayDetails
            ->findRectangleForOverlayProjection(
                RasterOverlayProjection::Geographic);
    ASSERT_NE(nullptr, contentCommittedRectangle);
    EXPECT_EQ(contentRasterRectangle, *contentCommittedRectangle);
}

TEST(TileLoadRequestDispatcherTest,
     RunsOnIssuedBeforeSynchronousContentUploadCallback) {
    std::mutex mutex;
    std::condition_variable condition;
    TilePendingRequestState requestState;
    TilePendingLoadQueue pendingLoads;
    FrameResourceBudgetConfig config;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);
    const TileKey key{"test", 0, 0, 0};
    bool issued = false;
    SyncRenderContentProvider provider(issued);

    TileLoadDispatchResult result =
        TileLoadRequestDispatcher::requestContent(
            mutex,
            condition,
            requestState,
            pendingLoads,
            budget,
            provider,
            key,
            "content-upload",
            TileLoadPriorityGroup::Normal,
            0.0,
            [&issued]() { issued = true; });

    EXPECT_EQ(TileLoadDispatchResult::Issued, result);
    EXPECT_TRUE(provider.callbackSawIssued);
    EXPECT_TRUE(requestState.empty());
    EXPECT_EQ(1u, pendingLoads.contentUploadCount());
    EXPECT_EQ(0u, pendingLoads.contentTerminalResultCount());
}

TEST(TileLoadRequestDispatcherTest,
     TerrainContentProviderQueuesTerrainContentDomain) {
    std::mutex mutex;
    std::condition_variable condition;
    TilePendingRequestState requestState;
    TilePendingLoadQueue pendingLoads;
    FrameResourceBudgetConfig config;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);
    const TileKey key{"test", 0, 0, 0};
    bool issued = false;
    SyncRenderTerrainContentProvider provider(issued);

    TileLoadDispatchResult result =
        TileLoadRequestDispatcher::requestContent(
            mutex,
            condition,
            requestState,
            pendingLoads,
            budget,
            provider,
            key,
            "gltf-terrain-upload",
            TileLoadPriorityGroup::Normal,
            0.0,
            [&issued]() { issued = true; });

    EXPECT_EQ(TileLoadDispatchResult::Issued, result);
    EXPECT_TRUE(provider.callbackSawIssued);
    EXPECT_TRUE(requestState.empty());
    EXPECT_EQ(1u, pendingLoads.gltfTerrainUploadCount());
    EXPECT_EQ(0u, pendingLoads.contentUploadCount());

    std::optional<PendingTileLoad> upload =
        pendingLoads.takeHighestPriorityUpload(budget);
    ASSERT_TRUE(upload.has_value());
    EXPECT_EQ(TileLoadDomain::TerrainContent, upload->domain);
    EXPECT_TRUE(upload->content().hasGltfTerrainPayload());
}

TEST(TileLoadRequestDispatcherTest,
     TerrainContentProviderRejectsOrdinaryRenderableGltfPayload) {
    std::mutex mutex;
    std::condition_variable condition;
    TilePendingRequestState requestState;
    TilePendingLoadQueue pendingLoads;
    FrameResourceBudgetConfig config;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);
    const TileKey key{"test", 0, 0, 0};
    bool issued = false;
    SyncOrdinaryRenderFromTerrainQuadtreeProvider provider(issued);

    TileLoadDispatchResult result =
        TileLoadRequestDispatcher::requestContent(
            mutex,
            condition,
            requestState,
            pendingLoads,
            budget,
            provider,
            key,
            "ordinary-gltf-from-terrain-provider",
            TileLoadPriorityGroup::Normal,
            0.0,
            [&issued]() { issued = true; });

    EXPECT_EQ(TileLoadDispatchResult::Issued, result);
    EXPECT_TRUE(provider.callbackSawIssued);
    EXPECT_TRUE(requestState.empty());
    EXPECT_EQ(0u, pendingLoads.gltfTerrainUploadCount());
    EXPECT_EQ(1u, pendingLoads.gltfTerrainTerminalResultCount());

    std::optional<PendingTileLoad> terminal =
        pendingLoads.takeHighestPriorityTerminalResult(budget);
    ASSERT_TRUE(terminal.has_value());
    EXPECT_EQ(TileLoadDomain::TerrainContent, terminal->domain);
    EXPECT_EQ(TileLoadStatus::Failed, terminal->result.status);
    EXPECT_FALSE(terminal->content().hasGltfTerrainPayload());
}

TEST(TileLoadRequestDispatcherTest,
     TerrainContentUpsampleQueuesFailedTerminalWithoutGltfPayload) {
    std::mutex mutex;
    TilePendingRequestState requestState;
    TilePendingLoadQueue pendingLoads;
    const TileKey key{"test", 1, 0, 0};

    TileLoadDispatchResult result =
        TileLoadRequestDispatcher::queueUpsampledLoad(
            mutex,
            requestState,
            pendingLoads,
            key,
            "empty-gltf-terrain-upsample",
            TileLoadPriorityGroup::Normal,
            0.0,
            TileLoadDomain::TerrainContent,
            makeMalformedRenderableWithoutPayloadForTest());

    EXPECT_EQ(TileLoadDispatchResult::Issued, result);
    EXPECT_EQ(0u, pendingLoads.gltfTerrainUploadCount());
    EXPECT_EQ(1u, pendingLoads.gltfTerrainTerminalResultCount());
    EXPECT_TRUE(pendingLoads.containsCacheKey(
        "empty-gltf-terrain-upsample"));

    FrameResourceBudgetConfig config;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);
    std::optional<PendingTileLoad> terminal =
        pendingLoads.takeHighestPriorityTerminalResult(budget);
    ASSERT_TRUE(terminal.has_value());
    EXPECT_EQ(TileLoadDomain::TerrainContent, terminal->domain);
    EXPECT_EQ(TileLoadStatus::Failed, terminal->result.status);
    EXPECT_FALSE(terminal->content().hasGltfTerrainPayload());
}

TEST(TileLoadRequestDispatcherTest, DropsCancelledContentTerminalCallback) {
    TileLoadLifecycle lifecycle;
    FrameResourceBudgetConfig config;
    config.maxNetworkRequestsPerFrame = 4;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);
    const TileKey key{"test", 0, 0, 0};
    bool issued = false;
    DeferredContentProvider provider;

    TileLoadDispatchResult result =
        TileLoadRequestDispatcher::requestContent(
            lifecycle.mutex(),
            lifecycle.condition(),
            lifecycle.requestState(),
            lifecycle.pendingLoads(),
            budget,
            provider,
            key,
            "cancel-content",
            TileLoadPriorityGroup::Normal,
            0.0,
            [&issued]() { issued = true; });

    ASSERT_EQ(TileLoadDispatchResult::Issued, result);
    ASSERT_TRUE(issued);
    ASSERT_TRUE(provider.contentCallback);

    lifecycle.cancelAndEraseCacheKey("cancel-content");
    provider.contentCallback(key, TileContentLoadResult::empty());

    // 未标 stale = 瓦片已销毁那条路:迟到结果整个丢弃。留下终态会让终态
    // 提交那一步的 ensureTile 把瓦片重新建出来。
    EXPECT_FALSE(lifecycle.hasPendingWork());
}

TEST(TileLoadRequestDispatcherTest, DropsCancelledContentRenderCallback) {
    TileLoadLifecycle lifecycle;
    FrameResourceBudgetConfig config;
    config.maxNetworkRequestsPerFrame = 4;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);
    const TileKey key{"test", 0, 0, 0};
    bool issued = false;
    DeferredContentProvider provider;

    TileLoadDispatchResult result =
        TileLoadRequestDispatcher::requestContent(
            lifecycle.mutex(),
            lifecycle.condition(),
            lifecycle.requestState(),
            lifecycle.pendingLoads(),
            budget,
            provider,
            key,
            "cancel-content-render",
            TileLoadPriorityGroup::Normal,
            0.0,
            [&issued]() { issued = true; });

    ASSERT_EQ(TileLoadDispatchResult::Issued, result);
    ASSERT_TRUE(issued);
    ASSERT_TRUE(provider.contentCallback);

    lifecycle.cancelAndEraseCacheKey("cancel-content-render");
    provider.contentCallback(
        key,
        TileContentLoadResult::render(std::make_unique<GltfModel>()));

    // 同上:瓦片已销毁,连已下载的 glTF 一起丢。
    EXPECT_FALSE(lifecycle.hasPendingWork());
}

TEST(TileLoadRequestDispatcherTest, DropsDestroyingContentUploadCallback) {
    TileLoadLifecycle lifecycle;
    FrameResourceBudgetConfig config;
    config.maxNetworkRequestsPerFrame = 4;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);
    const TileKey key{"test", 0, 0, 0};
    DeferredContentProvider provider;

    TileLoadDispatchResult result =
        TileLoadRequestDispatcher::requestContent(
            lifecycle.mutex(),
            lifecycle.condition(),
            lifecycle.requestState(),
            lifecycle.pendingLoads(),
            budget,
            provider,
            key,
            "destroy-content-upload",
            TileLoadPriorityGroup::Normal,
            0.0,
            []() {});

    ASSERT_EQ(TileLoadDispatchResult::Issued, result);
    ASSERT_TRUE(provider.contentCallback);

    {
        std::lock_guard<std::mutex> lock(lifecycle.mutex());
        lifecycle.requestState().markDestroyingAndCancelRequests();
    }

    provider.contentCallback(
        key,
        TileContentLoadResult::render(std::make_unique<GltfModel>()));

    EXPECT_FALSE(lifecycle.hasPendingWork());

    {
        std::lock_guard<std::mutex> lock(lifecycle.mutex());
        lifecycle.requestState().clearAfterCallbacksComplete();
    }
}

TEST(TileLoadRequestDispatcherTest, DropsDestroyingContentTerminalCallback) {
    TileLoadLifecycle lifecycle;
    FrameResourceBudgetConfig config;
    config.maxNetworkRequestsPerFrame = 4;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);
    const TileKey key{"test", 0, 0, 0};
    DeferredContentProvider provider;

    TileLoadDispatchResult result =
        TileLoadRequestDispatcher::requestContent(
            lifecycle.mutex(),
            lifecycle.condition(),
            lifecycle.requestState(),
            lifecycle.pendingLoads(),
            budget,
            provider,
            key,
            "destroy-content-terminal",
            TileLoadPriorityGroup::Normal,
            0.0,
            []() {});

    ASSERT_EQ(TileLoadDispatchResult::Issued, result);
    ASSERT_TRUE(provider.contentCallback);

    {
        std::lock_guard<std::mutex> lock(lifecycle.mutex());
        lifecycle.requestState().markDestroyingAndCancelRequests();
    }

    provider.contentCallback(key, TileContentLoadResult::retryLater());

    EXPECT_FALSE(lifecycle.hasPendingWork());

    {
        std::lock_guard<std::mutex> lock(lifecycle.mutex());
        lifecycle.requestState().clearAfterCallbacksComplete();
    }
}

TEST(TileLoadRequestDispatcherTest, SkipsPendingContentTerminalKeys) {
    std::mutex mutex;
    std::condition_variable condition;
    TilePendingRequestState requestState;
    TilePendingLoadQueue pendingLoads;
    FrameResourceBudgetConfig config;
    config.maxNetworkRequestsPerFrame = 4;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);
    const TileKey key{"test", 0, 0, 0};
    bool issued = false;
    SyncTerminalContentProvider provider(issued);

    TileLoadDispatchResult first =
        TileLoadRequestDispatcher::requestContent(
            mutex,
            condition,
            requestState,
            pendingLoads,
            budget,
            provider,
            key,
            "content-terminal",
            TileLoadPriorityGroup::Normal,
            0.0,
            [&issued]() { issued = true; });
    TileLoadDispatchResult second =
        TileLoadRequestDispatcher::requestContent(
            mutex,
            condition,
            requestState,
            pendingLoads,
            budget,
            provider,
            key,
            "content-terminal",
            TileLoadPriorityGroup::Normal,
            0.0,
            []() {});

    EXPECT_EQ(TileLoadDispatchResult::Issued, first);
    EXPECT_EQ(TileLoadDispatchResult::Skipped, second);
    EXPECT_TRUE(provider.callbackSawIssued);
    EXPECT_EQ(1u, pendingLoads.contentTerminalResultCount());
    EXPECT_EQ(1u, budget.networkRequestsIssued());
}

TEST(TileLoadRequestDispatcherTest,
     SkipsTerrainContentUpsampleWhenCacheKeyPending) {
    std::mutex mutex;
    TilePendingRequestState requestState;
    TilePendingLoadQueue pendingLoads;
    const TileKey key{"test", 0, 0, 0};

    {
        std::lock_guard<std::mutex> lock(mutex);
        pendingLoads.addTerminalResult(
            PendingTileLoad{
                TileLoadDomain::TerrainContent,
                key,
                "shared-cache-key",
                TileLoadPriorityGroup::Normal,
                0.0,
                TileLoadStatus::RetryLater});
    }

    TileLoadDispatchResult result =
        TileLoadRequestDispatcher::queueUpsampledLoad(
            mutex,
            requestState,
            pendingLoads,
            key,
            "shared-cache-key",
            TileLoadPriorityGroup::Normal,
            0.0,
            TileLoadDomain::TerrainContent,
            makeMalformedRenderableWithoutPayloadForTest());

    EXPECT_EQ(TileLoadDispatchResult::Skipped, result);
    EXPECT_EQ(0u, pendingLoads.gltfTerrainUploadCount());
    EXPECT_EQ(1u, pendingLoads.gltfTerrainTerminalResultCount());
}

TEST(TileLoadRequestDispatcherTest,
     TerrainContentUpsampleTerminalDoesNotConsumeNetworkBudget) {
    std::mutex mutex;
    TilePendingRequestState requestState;
    TilePendingLoadQueue pendingLoads;
    FrameResourceBudgetConfig config;
    config.maxNetworkRequestsPerFrame = 0;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);
    const TileKey key{"test", 0, 0, 0};

    TileLoadDispatchResult result =
        TileLoadRequestDispatcher::queueUpsampledLoad(
            mutex,
            requestState,
            pendingLoads,
            key,
            "upsample-blocked",
            TileLoadPriorityGroup::Urgent,
            100.0,
            TileLoadDomain::TerrainContent,
            makeMalformedRenderableWithoutPayloadForTest());

    EXPECT_EQ(TileLoadDispatchResult::Issued, result);
    EXPECT_TRUE(requestState.empty());
    EXPECT_EQ(0u, pendingLoads.gltfTerrainUploadCount());
    EXPECT_EQ(1u, pendingLoads.gltfTerrainTerminalResultCount());
    EXPECT_EQ(0u, budget.networkRequestsIssued());
}

TEST(TileLoadRequestDispatcherTest,
     QueuesUpsampledContentInContentDomainExplicitly) {
    std::mutex mutex;
    TilePendingRequestState requestState;
    TilePendingLoadQueue pendingLoads;
    const TileKey key{"test", 1, 0, 0};

    TileLoadDispatchResult result =
        TileLoadRequestDispatcher::queueUpsampledLoad(
            mutex,
            requestState,
            pendingLoads,
            key,
            "content-upsample",
            TileLoadPriorityGroup::Urgent,
            100.0,
            TileLoadDomain::Content,
            TileLoadResult::createRenderableGltfTerrain(
                std::make_unique<GltfModel>()));

    EXPECT_EQ(TileLoadDispatchResult::Issued, result);
    EXPECT_TRUE(requestState.empty());
    EXPECT_EQ(0u, pendingLoads.gltfTerrainUploadCount());
    EXPECT_EQ(1u, pendingLoads.contentUploadCount());
}

// === stale 差集回收取消:必须留终态,否则瓦片永久卡在 ContentLoading ===
//
// 真机 2026-08-09(25000m 冷启动):sweepStaleRequests 一次取消 35~54 个地形
// 请求后,registry 里 80 块瓦片恒在 ContentLoading、failTemp 恒 0、此后一个
// 请求都不再发,地形与影像都没上屏,而引擎报 pending=0「已收敛」。根因是取消
// 时结果被整个丢弃:请求侧记账被 completeContentRequest 清掉,瓦片侧却没人推
// 它离开 ContentLoading,调度器看它"还在加载"就永远不再请求。
//
// 与上面两个 Drops* 用例成对:那两个是**瓦片已销毁**(不标 stale)必须整个
// 丢弃;这两个是**瓦片还在**(标了 stale)必须留终态。两条路都经
// cancelAndErase 一个出口,靠标记区分。

TEST(TileLoadRequestDispatcherTest, StaleCancelledContentLeavesTerminalState) {
    TileLoadLifecycle lifecycle;
    FrameResourceBudgetConfig config;
    config.maxNetworkRequestsPerFrame = 4;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);
    const TileKey key{"test", 0, 0, 0};
    bool issued = false;
    DeferredContentProvider provider;

    ASSERT_EQ(TileLoadDispatchResult::Issued,
              TileLoadRequestDispatcher::requestContent(
                  lifecycle.mutex(),
                  lifecycle.condition(),
                  lifecycle.requestState(),
                  lifecycle.pendingLoads(),
                  budget,
                  provider,
                  key,
                  "stale-content",
                  TileLoadPriorityGroup::Normal,
                  0.0,
                  [&issued]() { issued = true; }));
    ASSERT_TRUE(issued);
    ASSERT_TRUE(provider.contentCallback);

    lifecycle.requestState().markStaleCancelled("stale-content");
    lifecycle.cancelAndEraseCacheKey("stale-content");
    provider.contentCallback(key, TileContentLoadResult::empty());

    EXPECT_EQ(1u, lifecycle.pendingLoads().terminalResultCount());
    EXPECT_EQ(0u, lifecycle.pendingLoads().uploadCount());
}

TEST(TileLoadRequestDispatcherTest, StaleCancelledRenderDropsPayloadNotState) {
    TileLoadLifecycle lifecycle;
    FrameResourceBudgetConfig config;
    config.maxNetworkRequestsPerFrame = 4;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);
    const TileKey key{"test", 0, 0, 0};
    bool issued = false;
    DeferredContentProvider provider;

    ASSERT_EQ(TileLoadDispatchResult::Issued,
              TileLoadRequestDispatcher::requestContent(
                  lifecycle.mutex(),
                  lifecycle.condition(),
                  lifecycle.requestState(),
                  lifecycle.pendingLoads(),
                  budget,
                  provider,
                  key,
                  "stale-render",
                  TileLoadPriorityGroup::Normal,
                  0.0,
                  [&issued]() { issued = true; }));
    ASSERT_TRUE(issued);

    lifecycle.requestState().markStaleCancelled("stale-render");
    lifecycle.cancelAndEraseCacheKey("stale-render");
    provider.contentCallback(
        key,
        TileContentLoadResult::render(std::make_unique<GltfModel>()));

    // 已下载的 glTF 仍然丢弃(不进 upload 车道)—— 我们已经决定不要它了;
    // 但终态要留下,让瓦片能退出 ContentLoading 并按退避重试。
    EXPECT_EQ(0u, lifecycle.pendingLoads().uploadCount());
    EXPECT_EQ(1u, lifecycle.pendingLoads().terminalResultCount());
}

TEST(TileLoadRequestDispatcherTest, StaleCancelMarkIsConsumedExactlyOnce) {
    TileLoadLifecycle lifecycle;
    lifecycle.requestState().markStaleCancelled("k");
    EXPECT_TRUE(lifecycle.requestState().takeStaleCancelled("k"));
    EXPECT_FALSE(lifecycle.requestState().takeStaleCancelled("k"));
}
