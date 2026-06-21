#include <gtest/gtest.h>

#include "earth_engine/content/GltfContentProvider.h"
#include "earth_engine/core/geodesy/Ellipsoid.h"
#include "earth_engine/core/resources/FrameResourceBudget.h"
#include "earth_engine/providers/TerrainProvider.h"
#include "earth_engine/renderer/RenderDevice.h"
#include "earth_engine/scene/Camera.h"
#include "earth_engine/scene/FrameState.h"
#include "earth_engine/scene/SceneTilesetDiagnostics.h"
#include "earth_engine/tiling/TileScheme.h"
#include "earth_engine/tiling/Tileset.h"

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace earth_engine;

namespace earth_engine {
struct TilesetTestAccess {
    static TilesetTile* ensureTile(Tileset& tileset, const TileKey& key) {
        return tileset.contentAccess_.ensureTile(key);
    }

    static TileLoadRequestOutcome requestMissingTilesWithBudget(
        Tileset& tileset,
        FrameResourceBudget& budget,
        const TileKey& firstKey,
        const TileKey& secondKey) {
        return tileset.requestMissingContent(
            {
                TileLoadRequest{
                    firstKey,
                    TileLoadPriorityGroup::Normal,
                    100.0},
                TileLoadRequest{
                    secondKey,
                    TileLoadPriorityGroup::Normal,
                    1.0},
            },
            &budget);
    }

    static void requestMissingTilesWithPriorities(
        Tileset& tileset,
        const TileKey& firstKey,
        double firstPriority,
        const TileKey& secondKey,
        double secondPriority) {
        tileset.requestMissingContent({
            TileLoadRequest{
                firstKey,
                TileLoadPriorityGroup::Normal,
                firstPriority},
            TileLoadRequest{
                secondKey,
                TileLoadPriorityGroup::Normal,
                secondPriority}});
    }

    static TilesetTile* findTile(Tileset& tileset, const TileKey& key) {
        return tileset.tileRegistry_.findTile(key);
    }

    static void processPendingUploads(Tileset& tileset) {
        tileset.processPendingContentUploads(false, false);
    }

    static void processPendingUploadsWithBudget(
        Tileset& tileset,
        FrameResourceBudget& budget) {
        tileset.processPendingContentUploads(false, false, &budget);
    }
};
} // namespace earth_engine

namespace {

class ManualCompletionTerrainProvider final : public TerrainProvider {
public:
    struct PendingRequest {
        TileKey key;
        HeightmapCallback callback;
    };

    std::string id() const override { return "manual-completion-terrain"; }
    std::string schemeId() const override { return "Geographic-TMS"; }
    int minZoom() const override { return 0; }
    int maxZoom() const override { return 1; }
    int tileSize() const override { return 2; }

    std::string buildUrl(const TileKey&) const override {
        return "memory://manual-completion-terrain";
    }

    void requestTile(
        const TileKey& key,
        CancellationToken,
        HeightmapCallback callback,
        HttpRequestPriority = HttpRequestPriority::Normal) override {
        pendingRequests.push_back(PendingRequest{key, std::move(callback)});
    }

    ProviderRequestDiagnostics requestDiagnostics() const override {
        ProviderRequestDiagnostics diagnostics;
        diagnostics.maximumTransportActiveRequests =
            maximumTransportActiveRequests;
        return diagnostics;
    }

    bool completeWithHeightmap(
        const TileKey& key,
        std::unique_ptr<DecodedHeightmap> heightmap) {
        auto it = std::find_if(
            pendingRequests.begin(),
            pendingRequests.end(),
            [&key](const PendingRequest& request) {
                return request.key == key;
            });
        if (it == pendingRequests.end()) {
            return false;
        }

        HeightmapCallback callback = std::move(it->callback);
        pendingRequests.erase(it);
        callback(key, TerrainTileLoadResult::success(std::move(heightmap)));
        return true;
    }

    std::unique_ptr<DecodedHeightmap> decodeTile(const uint8_t*, size_t)
        override {
        return nullptr;
    }

    std::vector<PendingRequest> pendingRequests;
    int maximumTransportActiveRequests = -1;
};

class ManualCompletionContentProvider final : public TilesetContentProvider {
public:
    struct PendingRequest {
        TileKey key;
        ContentCallback callback;
    };

    explicit ManualCompletionContentProvider(TileKey key)
        : key_(std::move(key)) {}

    std::string id() const override { return "manual-completion-content"; }
    bool supportsTile(const TileKey& key) const override { return key == key_; }
    std::vector<TileKey> rootTiles() const override { return {key_}; }

    void requestTileContent(
        const TileKey& key,
        CancellationToken,
        ContentCallback callback,
        HttpRequestPriority = HttpRequestPriority::Normal) override {
        pendingRequests.push_back(PendingRequest{key, std::move(callback)});
    }

    bool completeWithModel(
        const TileKey& key,
        std::unique_ptr<GltfModel> model) {
        auto it = std::find_if(
            pendingRequests.begin(),
            pendingRequests.end(),
            [&key](const PendingRequest& request) {
                return request.key == key;
            });
        if (it == pendingRequests.end()) {
            return false;
        }

        ContentCallback callback = std::move(it->callback);
        pendingRequests.erase(it);
        callback(key, TileContentLoadResult::render(std::move(model)));
        return true;
    }

    TileContentLoadResult decodeContent(const uint8_t*, size_t) override {
        return TileContentLoadResult::failed();
    }

    ProviderRequestDiagnostics requestDiagnostics() const override {
        return diagnostics;
    }

    std::vector<PendingRequest> pendingRequests;
    ProviderRequestDiagnostics diagnostics;

private:
    TileKey key_;
};

class DummyBuffer final : public Buffer {
public:
    explicit DummyBuffer(size_t byteSize) : byteSize_(byteSize) {}
    DummyBuffer(size_t byteSize, const void*) : byteSize_(byteSize) {}
    size_t size() const override { return byteSize_; }

private:
    size_t byteSize_ = 0;
};

class DummyShaderProgram final : public ShaderProgram {};

class DummyTexture final : public Texture {
public:
    DummyTexture(int width, int height) : width_(width), height_(height) {}
    int width() const override { return width_; }
    int height() const override { return height_; }

private:
    int width_ = 0;
    int height_ = 0;
};

class DummyRenderDevice final : public RenderDevice {
public:
    Backend backendType() const override { return Backend::OpenGLES; }
    int maxTextureSize() const override { return 4096; }
    int maxDrawBuffers() const override { return 4; }
    bool supportsFloatTextures() const override { return true; }
    bool supportsInstancing() const override { return true; }
    std::string rendererString() const override { return "DummyRenderDevice"; }

    std::unique_ptr<Texture> createTexture(const TextureDesc& desc) override {
        return std::make_unique<DummyTexture>(desc.width, desc.height);
    }

    bool updateTextureRegion(
        Texture*,
        int,
        int,
        int,
        int,
        const uint8_t*,
        size_t) override {
        return false;
    }

    std::unique_ptr<Buffer> createBuffer(const BufferDesc& desc) override {
        return std::make_unique<DummyBuffer>(desc.size, desc.data);
    }

    bool updateBuffer(Buffer*, size_t, const void*, size_t) override {
        return false;
    }

    std::unique_ptr<ShaderProgram> createShader(const ShaderDesc&) override {
        return std::make_unique<DummyShaderProgram>();
    }

    std::unique_ptr<Framebuffer> createFramebuffer(
        const FramebufferDesc&) override {
        return nullptr;
    }

    void beginFrame() override {}
    void submit(const RenderCommandList&) override {}
    void endFrame() override {}
    void onSurfaceCreated() override {}
    void onSurfaceChanged(int, int) override {}
    void onSurfaceDestroyed() override {}
};

std::unique_ptr<DecodedHeightmap> makeFlatHeightmap(float heightMeters) {
    auto heightmap = std::make_unique<DecodedHeightmap>();
    heightmap->tileSize = 2;
    heightmap->heights = {heightMeters, heightMeters, heightMeters, heightMeters};
    heightmap->minHeight = heightMeters;
    heightmap->maxHeight = heightMeters;
    return heightmap;
}

std::unique_ptr<GltfModel> makeTriangleGltfModel() {
    auto model = std::make_unique<GltfModel>();
    GltfPrimitive primitive;
    primitive.vertices.resize(3);
    primitive.vertices[0].positionEcef = Vec3(0.0, 0.0, 0.0);
    primitive.vertices[1].positionEcef = Vec3(1.0, 0.0, 0.0);
    primitive.vertices[2].positionEcef = Vec3(0.0, 1.0, 0.0);
    primitive.vertices[0].normalEcef = Vec3::unitZ();
    primitive.vertices[1].normalEcef = Vec3::unitZ();
    primitive.vertices[2].normalEcef = Vec3::unitZ();
    primitive.vertices[0].uv = {0.0f, 0.0f};
    primitive.vertices[1].uv = {1.0f, 0.0f};
    primitive.vertices[2].uv = {0.0f, 1.0f};
    primitive.indices = {0, 1, 2};
    model->primitives.push_back(std::move(primitive));
    return model;
}

SelectorView makeSelectorView(
    const Camera& camera,
    int viewportWidth,
    int viewportHeight) {
    SelectorView view;
    view.position = camera.position();
    view.direction = camera.direction();
    const double width = static_cast<double>(viewportWidth);
    const double height = static_cast<double>(viewportHeight);
    view.projectionMatrix = camera.projectionMatrix(width, height);
    view.frustum = Frustum::fromViewProjection(
        view.projectionMatrix * camera.viewMatrix());
    view.viewportHeightPixels = viewportHeight;
    return view;
}

} // namespace

TEST(TilesetRequestMissingBudgetTest,
     FrameResourceBudgetLimitsWorkerRequestsByPriority) {
    TilesetOptions options;
    options.maximumSimultaneousTileLoads = 2;

    auto provider = std::make_unique<ManualCompletionTerrainProvider>();
    ManualCompletionTerrainProvider* rawProvider = provider.get();
    Tileset tileset(
        std::move(provider),
        TileScheme::createGeographicTMS(),
        {},
        nullptr,
        options);

    const TileKey lowPriorityKey{"Geographic-TMS", 1, 0, 0};
    const TileKey highPriorityKey{"Geographic-TMS", 1, 1, 0};
    TilesetTestAccess::ensureTile(tileset, lowPriorityKey);
    TilesetTestAccess::ensureTile(tileset, highPriorityKey);

    FrameResourceBudgetConfig config;
    config.maxNetworkRequestsPerFrame = 1;
    config.maxNetworkInflight = 2;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);

    TileLoadRequestOutcome outcome =
        TilesetTestAccess::requestMissingTilesWithBudget(
            tileset,
            budget,
            lowPriorityKey,
            highPriorityKey);

    ASSERT_EQ(rawProvider->pendingRequests.size(), 1u);
    EXPECT_EQ(rawProvider->pendingRequests.front().key, highPriorityKey);
    EXPECT_EQ(outcome.issued, 1u);
    EXPECT_FALSE(outcome.blockedByInflight);
    EXPECT_EQ(budget.networkRequestsIssued(), 1u);

    EXPECT_TRUE(rawProvider->completeWithHeightmap(
        highPriorityKey,
        makeFlatHeightmap(2.0f)));
}

TEST(
    TilesetRequestMissingBudgetTest,
    FrameResourceBudgetLimitsMainThreadFinalizesByPriority) {
    TilesetOptions options;
    options.maximumSimultaneousTileLoads = 2;
    options.mainThreadLoadingTimeLimit = 0.0;

    auto provider = std::make_unique<ManualCompletionTerrainProvider>();
    ManualCompletionTerrainProvider* rawProvider = provider.get();
    Tileset tileset(
        std::move(provider),
        TileScheme::createGeographicTMS(),
        {},
        nullptr,
        options);

    const TileKey lowPriorityKey{"Geographic-TMS", 1, 0, 0};
    const TileKey highPriorityKey{"Geographic-TMS", 1, 1, 0};
    TilesetTestAccess::ensureTile(tileset, lowPriorityKey);
    TilesetTestAccess::ensureTile(tileset, highPriorityKey);
    TilesetTestAccess::requestMissingTilesWithPriorities(
        tileset,
        lowPriorityKey,
        100.0,
        highPriorityKey,
        1.0);

    ASSERT_EQ(rawProvider->pendingRequests.size(), 2u);
    EXPECT_TRUE(rawProvider->completeWithHeightmap(
        lowPriorityKey,
        makeFlatHeightmap(1.0f)));
    EXPECT_TRUE(rawProvider->completeWithHeightmap(
        highPriorityKey,
        makeFlatHeightmap(2.0f)));

    FrameResourceBudgetConfig config;
    config.maxMainThreadFinalizesPerFrame = 1;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);
    TilesetTestAccess::processPendingUploadsWithBudget(tileset, budget);

    TilesetTile* lowPriorityTile =
        TilesetTestAccess::findTile(tileset, lowPriorityKey);
    TilesetTile* highPriorityTile =
        TilesetTestAccess::findTile(tileset, highPriorityKey);
    const TilesetLoadDiagnostics diagnostics = tileset.loadDiagnostics();
    ASSERT_NE(highPriorityTile, nullptr);
    EXPECT_EQ(highPriorityTile->content.loadState, TileLoadState::Done);
    EXPECT_EQ(highPriorityTile->content.contentKind, TileContentKind::Render);
    ASSERT_NE(lowPriorityTile, nullptr);
    EXPECT_EQ(
        lowPriorityTile->content.loadState,
        TileLoadState::ContentLoading);
    EXPECT_EQ(diagnostics.pendingTerrainUploads, 1);
}

TEST(TilesetRequestMissingBudgetTest,
     MainThreadUploadBudgetIsGlobalAcrossContentKinds) {
    TilesetOptions options;
    options.maximumSimultaneousTileLoads = 2;
    options.mainThreadLoadingTimeLimit = 1.0e-12;

    const TileKey terrainKey{"Geographic-TMS", 1, 0, 0};
    const TileKey contentKey{"Geographic-TMS", 1, 1, 0};

    auto terrainProvider = std::make_unique<ManualCompletionTerrainProvider>();
    ManualCompletionTerrainProvider* rawTerrainProvider =
        terrainProvider.get();
    auto contentProvider =
        std::make_unique<ManualCompletionContentProvider>(contentKey);
    ManualCompletionContentProvider* rawContentProvider =
        contentProvider.get();
    DummyRenderDevice device;
    Tileset tileset(
        std::move(terrainProvider),
        TileScheme::createGeographicTMS(),
        {},
        &device,
        options,
        std::move(contentProvider));

    TilesetTestAccess::ensureTile(tileset, terrainKey);
    TilesetTestAccess::ensureTile(tileset, contentKey);
    TilesetTestAccess::requestMissingTilesWithPriorities(
        tileset,
        terrainKey,
        100.0,
        contentKey,
        1.0);

    ASSERT_EQ(rawTerrainProvider->pendingRequests.size(), 1u);
    ASSERT_EQ(rawContentProvider->pendingRequests.size(), 1u);
    EXPECT_TRUE(rawTerrainProvider->completeWithHeightmap(
        terrainKey,
        makeFlatHeightmap(1.0f)));
    EXPECT_TRUE(rawContentProvider->completeWithModel(
        contentKey,
        makeTriangleGltfModel()));
    EXPECT_EQ(tileset.loadDiagnostics().pendingTerrainUploads, 1);
    EXPECT_EQ(tileset.loadDiagnostics().pendingContentUploads, 1);

    TilesetTestAccess::processPendingUploads(tileset);

    TilesetTile* terrainTile =
        TilesetTestAccess::findTile(tileset, terrainKey);
    TilesetTile* contentTile =
        TilesetTestAccess::findTile(tileset, contentKey);
    ASSERT_NE(contentTile, nullptr);
    EXPECT_EQ(contentTile->content.loadState, TileLoadState::Done);
    EXPECT_EQ(contentTile->content.contentKind, TileContentKind::Render);
    EXPECT_TRUE(contentTile->content.renderContent.hasGltfModel());
    EXPECT_TRUE(contentTile->content.renderContent.hasGltfPrimitiveResources());

    const TilesetLoadDiagnostics diagnostics = tileset.loadDiagnostics();
    ASSERT_NE(terrainTile, nullptr);
    EXPECT_EQ(terrainTile->content.loadState, TileLoadState::ContentLoading);
    EXPECT_EQ(diagnostics.pendingTerrainUploads, 1);
    EXPECT_EQ(diagnostics.pendingContentUploads, 0);
}

TEST(
    TilesetRequestMissingBudgetTest,
    UpdateFrameUsesProviderTransportLaneForRasterBudget) {
    TilesetOptions options;
    options.maximumSimultaneousTileLoads = 20;

    auto provider = std::make_unique<ManualCompletionTerrainProvider>();
    provider->maximumTransportActiveRequests = 11;
    ManualCompletionTerrainProvider* rawProvider = provider.get();
    Tileset tileset(
        std::move(provider),
        TileScheme::createGeographicTMS(),
        {},
        nullptr,
        options);

    Camera camera;
    camera.lookAt(
        Vec3(Ellipsoid::WGS84().semiMajorAxis() * 2.0, 0.0, 0.0),
        Vec3(Ellipsoid::WGS84().semiMajorAxis(), 0.0, 0.0),
        Vec3::unitZ());

    FrameState frameState;
    frameState.frameId = 401;
    frameState.camera = &camera;
    frameState.viewportWidthPixels = 800;
    frameState.viewportHeightPixels = 800;
    frameState.selectorViews.push_back(makeSelectorView(camera, 800, 800));

    tileset.update(frameState);

    const TilesetLoadDiagnostics diagnostics = tileset.loadDiagnostics();
    EXPECT_EQ(diagnostics.resourceBudget.maxRasterNetworkRequestsPerFrame, 11u);
    EXPECT_EQ(diagnostics.resourceBudget.maxNetworkRequestsPerFrame, 20u);
    EXPECT_EQ(
        diagnostics.resourceBudget.maxTerrainContentNetworkRequestsPerFrame,
        20u);
    EXPECT_EQ(
        diagnostics.terrainProviderRequests.maximumTransportActiveRequests,
        11);

    while (!rawProvider->pendingRequests.empty()) {
        const TileKey pendingKey = rawProvider->pendingRequests.front().key;
        EXPECT_TRUE(rawProvider->completeWithHeightmap(
            pendingKey,
            makeFlatHeightmap(0.0f)));
    }
}

TEST(
    TilesetRequestMissingBudgetTest,
    LoadDiagnosticsExposeContentProviderRequestDiagnostics) {
    const TileKey contentKey{"Geographic-TMS", 0, 0, 0};
    auto contentProvider =
        std::make_unique<ManualCompletionContentProvider>(contentKey);
    contentProvider->diagnostics.requestsStarted = 1;
    contentProvider->diagnostics.requestsCompleted = 0;
    contentProvider->diagnostics.activeWorkerBlockingRequests = 0;
    contentProvider->diagnostics.peakWorkerBlockingRequests = 0;
    contentProvider->diagnostics.maximumTransportActiveRequests = 11;
    ManualCompletionContentProvider* rawContentProvider =
        contentProvider.get();

    Tileset tileset(
        std::unique_ptr<TerrainProvider>{},
        TileScheme::createGeographicTMS(),
        {},
        nullptr,
        TilesetOptions{},
        std::move(contentProvider));

    const TilesetLoadDiagnostics activeDiagnostics =
        tileset.loadDiagnostics();
    EXPECT_EQ(activeDiagnostics.contentProviderRequests.requestsStarted, 1);
    EXPECT_EQ(activeDiagnostics.contentProviderRequests.requestsCompleted, 0);
    EXPECT_EQ(
        activeDiagnostics
            .contentProviderRequests
            .activeWorkerBlockingRequests,
        0);
    EXPECT_EQ(
        activeDiagnostics
            .contentProviderRequests
            .peakWorkerBlockingRequests,
        0);
    EXPECT_EQ(
        activeDiagnostics
            .contentProviderRequests
            .maximumTransportActiveRequests,
        11);

    Diagnostics sceneDiagnostics;
    SceneTilesetDiagnostics::reset(sceneDiagnostics);
    SceneTilesetDiagnostics::addTileset(sceneDiagnostics, tileset, false);
    EXPECT_EQ(sceneDiagnostics.contentProviderRequestsStarted, 1);
    EXPECT_EQ(sceneDiagnostics.contentProviderRequestsCompleted, 0);
    EXPECT_EQ(sceneDiagnostics.contentProviderActiveWorkerBlockingRequests, 0);
    EXPECT_EQ(sceneDiagnostics.contentProviderPeakWorkerBlockingRequests, 0);
    EXPECT_EQ(sceneDiagnostics.contentTransportActiveRequestLimit, 11);

    rawContentProvider->diagnostics.requestsCompleted = 1;
    const TilesetLoadDiagnostics doneDiagnostics = tileset.loadDiagnostics();
    EXPECT_EQ(doneDiagnostics.contentProviderRequests.requestsCompleted, 1);
    EXPECT_EQ(
        doneDiagnostics
            .contentProviderRequests
            .activeWorkerBlockingRequests,
        0);
    EXPECT_EQ(
        doneDiagnostics
            .contentProviderRequests
            .peakWorkerBlockingRequests,
        0);
}

TEST(
    TilesetRequestMissingBudgetTest,
    FrameResourceBudgetSeparatesRasterFanoutFromTerrainRequests) {
    TilesetOptions options;
    options.maximumSimultaneousTileLoads = 2;

    auto provider = std::make_unique<ManualCompletionTerrainProvider>();
    ManualCompletionTerrainProvider* rawProvider = provider.get();
    Tileset tileset(
        std::move(provider),
        TileScheme::createGeographicTMS(),
        {},
        nullptr,
        options);

    const TileKey lowPriorityKey{"Geographic-TMS", 1, 0, 0};
    const TileKey highPriorityKey{"Geographic-TMS", 1, 1, 0};
    TilesetTestAccess::ensureTile(tileset, lowPriorityKey);
    TilesetTestAccess::ensureTile(tileset, highPriorityKey);

    FrameResourceBudgetConfig config;
    config.maxNetworkRequestsPerFrame = 1;
    config.maxTerrainContentNetworkRequestsPerFrame = 1;
    config.maxRasterNetworkRequestsPerFrame = 4;
    config.maxNetworkInflight = 2;
    config.maxTerrainContentNetworkInflight = 2;
    config.maxRasterNetworkInflight = 4;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);

    ASSERT_TRUE(budget.tryIssue(
        FrameResourceLane::RasterRequest,
        FrameResourcePriority::Normal,
        4));

    TileLoadRequestOutcome outcome =
        TilesetTestAccess::requestMissingTilesWithBudget(
            tileset,
            budget,
            lowPriorityKey,
            highPriorityKey);

    ASSERT_EQ(rawProvider->pendingRequests.size(), 1u);
    EXPECT_EQ(rawProvider->pendingRequests.front().key, highPriorityKey);
    EXPECT_EQ(outcome.issued, 1u);
    EXPECT_FALSE(outcome.blockedByInflight);
    EXPECT_EQ(budget.rasterNetworkRequestsIssued(), 4u);
    EXPECT_EQ(budget.terrainContentNetworkRequestsIssued(), 1u);
    EXPECT_EQ(budget.networkRequestsIssued(), 5u);

    EXPECT_TRUE(rawProvider->completeWithHeightmap(
        highPriorityKey,
        makeFlatHeightmap(2.0f)));
}
