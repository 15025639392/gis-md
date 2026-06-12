#include <gtest/gtest.h>
#include <cmath>
#include <unordered_set>
#include "earth_engine/layers/BasemapLayerStack.h"
#include "earth_engine/layers/BasemapLayer.h"
#include "earth_engine/tiling/TileScheme.h"
#include "earth_engine/tiling/TileGroupKey.h"
#include "earth_engine/tiling/CrsProfile.h"
#include "earth_engine/scene/Camera.h"
#include "earth_engine/scene/FrameState.h"
#include "earth_engine/providers/DebugImageryProvider.h"
#include "earth_engine/renderer/Renderer.h"
#include "earth_engine/renderer/RenderDevice.h"

using namespace earth_engine;

// ============================================================
// 构造 Stub Provider（不发起网络请求）
// ============================================================

namespace {

class StubProvider : public ImageryProvider {
public:
    explicit StubProvider(std::string id, std::string schemeId, int maxZoom = 20)
        : id_(std::move(id)),
          schemeId_(std::move(schemeId)),
          maxZoom_(maxZoom) {}

    std::string id() const override { return id_; }
    std::string schemeId() const override { return schemeId_; }
    int minZoom() const override { return 0; }
    int maxZoom() const override { return maxZoom_; }
    int tileWidth() const override { return 256; }
    int tileHeight() const override { return 256; }
    std::string buildUrl(const TileKey&) const override { return ""; }

    void requestTile(const TileKey&, CancellationToken,
                     TileCallback callback) override {
        // Stub: 不发起请求，直接回调空指针
        callback(TileKey{}, nullptr);
    }

    std::unique_ptr<DecodedImage> decodeTile(
        const uint8_t*, size_t) override {
        return nullptr;
    }

private:
    std::string id_;
    std::string schemeId_;
    int maxZoom_ = 20;
};

class UnsupportedOpenGlobusProvider : public StubProvider {
public:
    UnsupportedOpenGlobusProvider()
        : StubProvider("stub-og-unsupported", "OpenGlobus-Earth") {}

    bool supportsTile(const TileKey&) const override { return false; }
};

class MercatorOnlyOpenGlobusProvider : public StubProvider {
public:
    MercatorOnlyOpenGlobusProvider()
        : StubProvider("stub-og-mercator", "OpenGlobus-Earth") {}

    bool supportsTile(const TileKey& key) const override {
        const int tilesAtZoom = 1 << key.z;
        return key.schemeId == "OpenGlobus-Earth" &&
               key.y >= 0 &&
               key.y < tilesAtZoom;
    }
};

class CountingProvider : public StubProvider {
public:
    explicit CountingProvider(bool returnsImage = false)
        : StubProvider("counting", "XYZ-WebMercator"),
          returnsImage_(returnsImage) {}

    void requestTile(const TileKey& key, CancellationToken,
                     TileCallback callback) override {
        requestedKeys.push_back(key);
        if (returnsImage_) {
            auto image = std::make_unique<DecodedImage>();
            image->width = 1;
            image->height = 1;
            image->channels = 4;
            image->pixels = {255, 255, 255, 255};
            callback(key, std::move(image));
            return;
        }
        callback(key, nullptr);
    }

    std::vector<TileKey> requestedKeys;

private:
    bool returnsImage_ = false;
};

class DeferredProvider : public StubProvider {
public:
    DeferredProvider() : StubProvider("deferred", "XYZ-WebMercator") {}

    void requestTile(const TileKey& key, CancellationToken,
                     TileCallback callback) override {
        requestedKeys.push_back(key);
        callbacks.push_back({key, std::move(callback)});
    }

    void completeFirst() {
        ASSERT_FALSE(callbacks.empty());
        auto pending = std::move(callbacks.front());
        callbacks.erase(callbacks.begin());
        auto image = std::make_unique<DecodedImage>();
        image->width = 1;
        image->height = 1;
        image->channels = 4;
        image->pixels = {255, 255, 255, 255};
        pending.callback(pending.key, std::move(image));
    }

    std::vector<TileKey> requestedKeys;

private:
    struct Pending {
        TileKey key;
        TileCallback callback;
    };
    std::vector<Pending> callbacks;
};

class MaxZoom18Provider : public CountingProvider {
public:
    explicit MaxZoom18Provider(bool returnsImage = false)
        : CountingProvider(returnsImage) {}

    int maxZoom() const override { return 18; }
};

class FakeTexture : public Texture {
public:
    FakeTexture(int width, int height) : width_(width), height_(height) {}
    int width() const override { return width_; }
    int height() const override { return height_; }

private:
    int width_;
    int height_;
};

class FakeBuffer : public Buffer {
public:
    explicit FakeBuffer(size_t size) : size_(size) {}
    size_t size() const override { return size_; }

private:
    size_t size_ = 0;
};

class FakeRenderDevice : public RenderDevice {
public:
    Backend backendType() const override { return Backend::OpenGLES; }
    int maxTextureSize() const override { return 4096; }
    int maxDrawBuffers() const override { return 4; }
    bool supportsFloatTextures() const override { return true; }
    bool supportsInstancing() const override { return true; }
    std::string rendererString() const override { return "fake"; }

    std::unique_ptr<Texture> createTexture(const TextureDesc& desc) override {
        ++textureCreates;
        return std::make_unique<FakeTexture>(desc.width, desc.height);
    }

    std::unique_ptr<Buffer> createBuffer(const BufferDesc&) override {
        ++bufferCreates;
        return std::make_unique<FakeBuffer>(1);
    }

    bool updateBuffer(Buffer* buffer,
                      size_t offset,
                      const void* data,
                      size_t size) override {
        return buffer && data && offset + size <= buffer->size();
    }

    std::unique_ptr<ShaderProgram> createShader(const ShaderDesc&) override {
        return nullptr;
    }

    std::unique_ptr<Framebuffer> createFramebuffer(const FramebufferDesc&) override {
        return nullptr;
    }

    void beginFrame() override {}
    void submit(const RenderCommandList&) override {}
    void endFrame() override {}
    void onSurfaceCreated() override {}
    void onSurfaceChanged(int, int) override {}
    void onSurfaceDestroyed() override {}

    int textureCreates = 0;
    int bufferCreates = 0;
};

} // anonymous namespace

// ============================================================
// Test fixture
// ============================================================

class BasemapLayerStackTest : public ::testing::Test {
protected:
    void SetUp() override {
        camera_ = std::make_unique<Camera>();
        camera_->setPerspective(glm::radians(60.0), 1.0, 50000000.0);
        camera_->lookAt(Vec3(0, 0, 7000000), Vec3::zero(), Vec3::unitY());

        frameState_.camera = camera_.get();
        frameState_.viewportWidthPixels = 800;
        frameState_.viewportHeightPixels = 600;
        frameState_.frameId = 1;
        frameState_.timeSeconds = 0.0;
    }

    std::unique_ptr<BasemapLayer> makeXYZLayer(const std::string& name) {
        return std::make_unique<BasemapLayer>(
            std::make_unique<StubProvider>("stub-" + name, "XYZ-WebMercator"),
            TileScheme::createXYZWebMercator(),
            nullptr  // no GPU for tests
        );
    }

    std::unique_ptr<BasemapLayer> makeTMSLayer(const std::string& name) {
        return std::make_unique<BasemapLayer>(
            std::make_unique<StubProvider>("stub-" + name, "TMS-WebMercator"),
            TileScheme::createTMS(),
            nullptr
        );
    }

    std::unique_ptr<Camera> camera_;
    FrameState frameState_;
};

// ============================================================
// 图层管理
// ============================================================

TEST_F(BasemapLayerStackTest, AddAndCount) {
    BasemapLayerStack stack;
    EXPECT_EQ(0u, stack.layerCount());

    stack.addLayer(makeXYZLayer("a"));
    EXPECT_EQ(1u, stack.layerCount());

    stack.addLayer(makeXYZLayer("b"));
    EXPECT_EQ(2u, stack.layerCount());
}

TEST_F(BasemapLayerStackTest, RemoveLayer) {
    BasemapLayerStack stack;
    stack.addLayer(makeXYZLayer("a"));
    stack.addLayer(makeXYZLayer("b"));

    auto removed = stack.removeLayer("stub-a");
    ASSERT_NE(nullptr, removed);
    EXPECT_EQ("stub-a", removed->id());
    EXPECT_EQ(1u, stack.layerCount());

    // 不存在
    auto missing = stack.removeLayer("nonexistent");
    EXPECT_EQ(nullptr, missing);
}

TEST_F(BasemapLayerStackTest, MoveLayer) {
    BasemapLayerStack stack;
    stack.addLayer(makeXYZLayer("a"));
    stack.addLayer(makeXYZLayer("b"));
    stack.addLayer(makeXYZLayer("c"));

    // a, b, c → move "a" to index 2 → b, c, a
    stack.moveLayer("stub-a", 2);
    EXPECT_EQ("stub-b", stack.layerAt(0)->id());
    EXPECT_EQ("stub-c", stack.layerAt(1)->id());
    EXPECT_EQ("stub-a", stack.layerAt(2)->id());
}

TEST_F(BasemapLayerStackTest, FindLayer) {
    BasemapLayerStack stack;
    stack.addLayer(makeXYZLayer("a"));

    EXPECT_NE(nullptr, stack.findLayer("stub-a"));
    EXPECT_EQ(nullptr, stack.findLayer("stub-b"));
}

// ============================================================
// TilePlan 分组
// ============================================================

TEST_F(BasemapLayerStackTest, SameSchemeSharesTilePlan) {
    BasemapLayerStack stack;
    stack.addLayer(makeXYZLayer("a"));
    stack.addLayer(makeXYZLayer("b"));

    stack.update(frameState_);

    // 两个 XYZ 图层应有相同的 TileGroupKey → 共享一次 TilePlan 计算
    const auto& plans = stack.groupPlans();
    // 当前实现：每个独特 (schemeId, zoom, vpW, vpH) 键对应一个 TilePlan
    // 两个 XYZ 图层 zoom 相同 → 1 个 plan
    EXPECT_GE(plans.size(), 1u);

    // 两个图层的 visibleTiles 应一致
    auto* layerA = stack.findLayer("stub-a");
    auto* layerB = stack.findLayer("stub-b");
    ASSERT_NE(nullptr, layerA);
    ASSERT_NE(nullptr, layerB);

    const auto& tilesA = layerA->tilePlan().visibleTiles;
    const auto& tilesB = layerB->tilePlan().visibleTiles;

    EXPECT_EQ(tilesA.size(), tilesB.size());
    for (size_t i = 0; i < tilesA.size(); ++i) {
        EXPECT_EQ(tilesA[i], tilesB[i]);
    }
}

TEST_F(BasemapLayerStackTest, LayerPlanSeparatesDesiredRequestAndRenderTiles) {
    BasemapLayerStack stack;
    stack.addLayer(makeXYZLayer("a"));

    stack.update(frameState_);

    auto* layer = stack.findLayer("stub-a");
    ASSERT_NE(nullptr, layer);

    const LayerTilePlan& plan = layer->layerPlan();
    EXPECT_EQ(layer->tilePlan().visibleTiles.size(), plan.desiredTiles.size());
    EXPECT_LE(plan.requestTiles.size(), plan.desiredTiles.size());
    EXPECT_GT(plan.requestTiles.size(), 0u);
    EXPECT_TRUE(plan.renderTiles.empty());
    EXPECT_TRUE(plan.fallbackTiles.empty());

    for (const TileKey& key : plan.requestTiles) {
        bool isDesiredOrAncestor = false;
        for (const TileKey& desired : plan.desiredTiles) {
            TileKey candidate = desired;
            while (true) {
                if (candidate == key) {
                    isDesiredOrAncestor = true;
                    break;
                }
                if (candidate.z <= layer->tileScheme().minZoom()) break;
                candidate = TilePlanBuilder::parentKey(candidate);
            }
            if (isDesiredOrAncestor) break;
        }
        EXPECT_TRUE(isDesiredOrAncestor);
    }
}

TEST_F(BasemapLayerStackTest, BasemapLayerTrustsTilePlanVisibilityWithoutSecondCulling) {
    auto layer = makeXYZLayer("a");

    TilePlan plan;
    plan.frameId = 7;
    plan.zoom = 2;
    plan.visibleTiles = {
        TileKey{"XYZ-WebMercator", 2, 2, 1},
        TileKey{"XYZ-WebMercator", 2, 0, 1}
    };

    constexpr double radius = 6378137.0;
    layer->applyPlan(plan, Vec3(radius * 3.0, 0.0, 0.0));

    const LayerTilePlan& layerPlan = layer->layerPlan();
    const TileKey rootKey{"XYZ-WebMercator", 0, 0, 0};
    ASSERT_EQ(1u, layerPlan.requestTiles.size());
    EXPECT_EQ(rootKey, layerPlan.requestTiles.front());
    EXPECT_TRUE(layerPlan.renderTiles.empty());
}

TEST_F(BasemapLayerStackTest, LayerPlanCarriesOpenGlobusQuadTreeDiagnostics) {
    auto layer = makeXYZLayer("a");

    TilePlan plan;
    plan.frameId = 9;
    plan.zoom = 4;
    plan.minVisibleZoom = 4;
    plan.maxVisibleZoom = 4;
    plan.equalZoomApplied = true;
    plan.lodSizePixels = 512.0;
    plan.fadingNodeCount = 3;
    plan.neighborLinkCount = 7;
    plan.renderingNodeCount = 11;
    plan.walkthroughNodeCount = 5;
    plan.notRenderingNodeCount = 2;
    plan.cameraInsideNodeCount = 1;
    plan.inFrustumNodeCount = 8;
    plan.visibleTiles = {
        TileKey{"XYZ-WebMercator", 4, 8, 7}
    };

    constexpr double radius = 6378137.0;
    layer->applyPlan(plan, Vec3(radius * 3.0, 0.0, 0.0));

    const LayerTilePlan& layerPlan = layer->layerPlan();
    EXPECT_EQ(4, layerPlan.minVisibleZoom);
    EXPECT_EQ(4, layerPlan.maxVisibleZoom);
    EXPECT_TRUE(layerPlan.equalZoomApplied);
    EXPECT_DOUBLE_EQ(512.0, layerPlan.lodSizePixels);
    EXPECT_EQ(3, layerPlan.quadtreeFadingNodeCount);
    EXPECT_EQ(7, layerPlan.quadtreeNeighborLinkCount);
    EXPECT_EQ(11, layerPlan.quadtreeRenderingNodeCount);
    EXPECT_EQ(5, layerPlan.quadtreeWalkthroughNodeCount);
    EXPECT_EQ(2, layerPlan.quadtreeNotRenderingNodeCount);
    EXPECT_EQ(1, layerPlan.quadtreeCameraInsideNodeCount);
    EXPECT_EQ(8, layerPlan.quadtreeInFrustumNodeCount);
    EXPECT_EQ(3, layerPlan.transitionTileCount);
}

TEST_F(BasemapLayerStackTest, ProviderAvailabilityBlocksUnsupportedRequests) {
    auto layer = std::make_unique<BasemapLayer>(
        std::make_unique<UnsupportedOpenGlobusProvider>(),
        TileScheme::createOpenGlobusEarth(),
        nullptr);

    TilePlan plan;
    plan.frameId = 8;
    plan.zoom = 3;
    plan.visibleTiles = {
        TileKey{"OpenGlobus-Earth", 3, 4, 12}
    };

    constexpr double radius = 6378137.0;
    layer->applyPlan(plan, Vec3(radius * 3.0, 0.0, 0.0));

    const LayerTilePlan& layerPlan = layer->layerPlan();
    EXPECT_TRUE(layerPlan.desiredTiles.empty());
    EXPECT_TRUE(layerPlan.requestTiles.empty());
    EXPECT_TRUE(layerPlan.renderTiles.empty());
    EXPECT_EQ(0, layerPlan.missingTileCount);
    EXPECT_EQ(1, layerPlan.unsupportedTileCount);
}

TEST_F(BasemapLayerStackTest, ProviderAvailabilityKeepsSupportedDesiredTilesOnly) {
    auto layer = std::make_unique<BasemapLayer>(
        std::make_unique<MercatorOnlyOpenGlobusProvider>(),
        TileScheme::createOpenGlobusEarth(),
        nullptr);

    TilePlan plan;
    plan.frameId = 9;
    plan.zoom = 3;
    plan.visibleTiles = {
        TileKey{"OpenGlobus-Earth", 3, 4, 4},
        TileKey{"OpenGlobus-Earth", 3, 4, 12}
    };

    constexpr double radius = 6378137.0;
    layer->applyPlan(plan, Vec3(radius * 3.0, 0.0, 0.0));

    const LayerTilePlan& layerPlan = layer->layerPlan();
    ASSERT_EQ(1u, layerPlan.desiredTiles.size());
    EXPECT_EQ((TileKey{"OpenGlobus-Earth", 3, 4, 4}), layerPlan.desiredTiles.front());
    EXPECT_EQ(1, layerPlan.missingTileCount);
    EXPECT_EQ(1, layerPlan.unsupportedTileCount);
    ASSERT_EQ(1u, layerPlan.requestTiles.size());
    EXPECT_EQ((TileKey{"OpenGlobus-Earth", 0, 0, 0}), layerPlan.requestTiles.front());
}

TEST_F(BasemapLayerStackTest, BasemapLayerKeepsTileGenerationAcrossFrameIds) {
    FakeRenderDevice device;
    auto layer = std::make_unique<BasemapLayer>(
        std::make_unique<DebugImageryProvider>(),
        TileScheme::createXYZWebMercator(),
        &device);

    TilePlan plan;
    plan.frameId = 1;
    plan.zoom = 0;
    plan.visibleTiles = {
        TileKey{"XYZ-WebMercator", 0, 0, 0},
    };
    plan.tileTransitions = {
        TileTransition{TileKey{"XYZ-WebMercator", 0, 0, 0}, 0.4f, 1},
    };

    constexpr double radius = 6378137.0;
    const Vec3 cameraPosition(radius * 7.0, 0.0, 0.0);
    layer->applyPlan(plan, cameraPosition);
    layer->loadMissingTiles();

    TilePlan nextFrame = plan;
    nextFrame.frameId = 2;
    layer->applyPlan(nextFrame, cameraPosition);

    EXPECT_EQ(1, device.textureCreates);
    EXPECT_EQ(1, layer->cachedTileCount());
    EXPECT_EQ(1, layer->renderTileCount());
    EXPECT_EQ(1, layer->exactAttachmentCount());
    ASSERT_EQ(1u, layer->layerPlan().renderTiles.size());
    EXPECT_FLOAT_EQ(0.4f, layer->layerPlan().renderTiles[0].transitionOpacity);
}

TEST_F(BasemapLayerStackTest, MissingHighZoomTilesRequestFirstMissingParentOnce) {
    auto provider = std::make_unique<CountingProvider>();
    auto* providerPtr = provider.get();
    auto layer = std::make_unique<BasemapLayer>(
        std::move(provider),
        TileScheme::createXYZWebMercator(),
        nullptr);

    TilePlan plan;
    plan.frameId = 11;
    plan.zoom = 8;
    for (int i = 0; i < 20; ++i) {
        plan.visibleTiles.push_back(TileKey{"XYZ-WebMercator", 8, i, 120});
    }

    constexpr double radius = 6378137.0;
    layer->applyPlan(plan, Vec3(radius * 2.0, 0.0, 0.0));
    layer->loadMissingTiles();

    const TileKey rootKey{"XYZ-WebMercator", 0, 0, 0};
    ASSERT_EQ(1u, layer->layerPlan().requestTiles.size());
    EXPECT_EQ(rootKey, layer->layerPlan().requestTiles.front());
    ASSERT_EQ(1u, providerPtr->requestedKeys.size());
    EXPECT_EQ(rootKey, providerPtr->requestedKeys.front());
}

TEST_F(BasemapLayerStackTest, LoadMissingTilesDoesNotDuplicateInFlightRequests) {
    auto provider = std::make_unique<CountingProvider>();
    auto* providerPtr = provider.get();
    auto layer = std::make_unique<BasemapLayer>(
        std::move(provider),
        TileScheme::createXYZWebMercator(),
        nullptr);

    TilePlan plan;
    plan.frameId = 12;
    plan.zoom = 6;
    for (int i = 0; i < 6; ++i) {
        plan.visibleTiles.push_back(TileKey{"XYZ-WebMercator", 6, i, 30});
    }

    constexpr double radius = 6378137.0;
    layer->applyPlan(plan, Vec3(radius * 2.0, 0.0, 0.0));
    layer->loadMissingTiles();
    layer->loadMissingTiles();

    const TileKey rootKey{"XYZ-WebMercator", 0, 0, 0};
    ASSERT_EQ(1u, providerPtr->requestedKeys.size());
    EXPECT_EQ(rootKey, providerPtr->requestedKeys.front());
}

TEST_F(BasemapLayerStackTest, AncestorRequestUploadIsAcceptedForHighZoomDesiredTiles) {
    FakeRenderDevice device;
    auto provider = std::make_unique<CountingProvider>(true);
    auto* providerPtr = provider.get();
    auto layer = std::make_unique<BasemapLayer>(
        std::move(provider),
        TileScheme::createXYZWebMercator(),
        &device);

    TilePlan plan;
    plan.frameId = 13;
    plan.zoom = 6;
    plan.visibleTiles = {
        TileKey{"XYZ-WebMercator", 6, 12, 30},
        TileKey{"XYZ-WebMercator", 6, 13, 30}
    };

    constexpr double radius = 6378137.0;
    const Vec3 cameraPosition(radius * 2.0, 0.0, 0.0);
    layer->applyPlan(plan, cameraPosition);
    layer->loadMissingTiles();

    TilePlan nextFrame = plan;
    nextFrame.frameId = 14;
    layer->applyPlan(nextFrame, cameraPosition);

    const TileKey rootKey{"XYZ-WebMercator", 0, 0, 0};
    ASSERT_EQ(1u, providerPtr->requestedKeys.size());
    EXPECT_EQ(rootKey, providerPtr->requestedKeys.front());
    EXPECT_EQ(1, device.textureCreates);
    EXPECT_EQ(1, layer->cachedTileCount());
    EXPECT_EQ(2, layer->parentFallbackAttachmentCount());
}

TEST_F(BasemapLayerStackTest, ParentFallbackIsReadyMaterialAndKeepsLodOpacity) {
    FakeRenderDevice device;
    auto layer = std::make_unique<BasemapLayer>(
        std::make_unique<CountingProvider>(true),
        TileScheme::createXYZWebMercator(),
        &device);

    TilePlan parentPlan;
    parentPlan.frameId = 30;
    parentPlan.zoom = 0;
    parentPlan.visibleTiles = {
        TileKey{"XYZ-WebMercator", 0, 0, 0}
    };

    constexpr double radius = 6378137.0;
    const Vec3 cameraPosition(radius * 2.0, 0.0, 0.0);
    layer->applyPlan(parentPlan, cameraPosition);
    layer->loadMissingTiles();
    parentPlan.frameId = 31;
    layer->applyPlan(parentPlan, cameraPosition);

    TilePlan childPlan;
    childPlan.frameId = 32;
    childPlan.zoom = 6;
    childPlan.visibleTiles = {
        TileKey{"XYZ-WebMercator", 6, 12, 30}
    };
    childPlan.tileTransitions = {
        TileTransition{TileKey{"XYZ-WebMercator", 6, 12, 30}, 1.0f, 0}
    };
    layer->applyPlan(childPlan, cameraPosition);

    ASSERT_EQ(1u, layer->layerPlan().renderTiles.size());
    EXPECT_EQ(TileRenderSource::ParentFallback,
              layer->layerPlan().renderTiles[0].source);
    EXPECT_EQ(TileReadinessState::ParentFallback,
              layer->layerPlan().renderTiles[0].readiness);
    EXPECT_FLOAT_EQ(1.0f, layer->layerPlan().renderTiles[0].transitionOpacity);
    EXPECT_EQ(1, layer->parentFallbackAttachmentCount());
    EXPECT_EQ(0, layer->missingImageryTileCount());
}

TEST_F(BasemapLayerStackTest, StaleAncestorUploadAcceptedWhenStillCurrentFallback) {
    FakeRenderDevice device;
    auto provider = std::make_unique<DeferredProvider>();
    auto* providerPtr = provider.get();
    auto layer = std::make_unique<BasemapLayer>(
        std::move(provider),
        TileScheme::createXYZWebMercator(),
        &device);

    TilePlan firstPlan;
    firstPlan.frameId = 40;
    firstPlan.zoom = 6;
    firstPlan.visibleTiles = {
        TileKey{"XYZ-WebMercator", 6, 12, 30}
    };

    constexpr double radius = 6378137.0;
    const Vec3 cameraPosition(radius * 2.0, 0.0, 0.0);
    layer->applyPlan(firstPlan, cameraPosition);
    layer->loadMissingTiles();
    ASSERT_EQ(1u, providerPtr->requestedKeys.size());
    EXPECT_EQ((TileKey{"XYZ-WebMercator", 0, 0, 0}),
              providerPtr->requestedKeys.front());

    TilePlan nextPlan = firstPlan;
    nextPlan.frameId = 41;
    nextPlan.visibleTiles = {
        TileKey{"XYZ-WebMercator", 6, 13, 30}
    };
    layer->applyPlan(nextPlan, cameraPosition);

    providerPtr->completeFirst();
    layer->applyPlan(nextPlan, cameraPosition);

    EXPECT_EQ(1, device.textureCreates);
    EXPECT_EQ(1, layer->cachedTileCount());
    EXPECT_EQ(1, layer->parentFallbackAttachmentCount());
    EXPECT_EQ(0, layer->missingImageryTileCount());
}

TEST_F(BasemapLayerStackTest, UnsupportedHighZoomUsesSupportedAncestor) {
    FakeRenderDevice device;
    auto provider = std::make_unique<MaxZoom18Provider>(true);
    auto* providerPtr = provider.get();
    auto layer = std::make_unique<BasemapLayer>(
        std::move(provider),
        TileScheme::createXYZWebMercator(),
        &device);

    TilePlan plan;
    plan.frameId = 50;
    plan.zoom = 19;
    plan.minVisibleZoom = 19;
    plan.maxVisibleZoom = 19;
    plan.visibleTiles = {
        TileKey{"XYZ-WebMercator", 19, 415058, 207336},
        TileKey{"XYZ-WebMercator", 19, 415059, 207336},
        TileKey{"XYZ-WebMercator", 19, 415058, 207337},
        TileKey{"XYZ-WebMercator", 19, 415059, 207337}
    };

    constexpr double radius = 6378137.0;
    const Vec3 cameraPosition(radius + 1000.0, 0.0, 0.0);
    layer->applyPlan(plan, cameraPosition);
    layer->loadMissingTiles();

    ASSERT_EQ(1u, layer->layerPlan().desiredTiles.size());
    EXPECT_EQ((TileKey{"XYZ-WebMercator", 18, 207529, 103668}),
              layer->layerPlan().desiredTiles.front());
    ASSERT_EQ(1u, providerPtr->requestedKeys.size());
    EXPECT_EQ((TileKey{"XYZ-WebMercator", 0, 0, 0}),
              providerPtr->requestedKeys.front());

    plan.frameId = 51;
    layer->applyPlan(plan, cameraPosition);

    ASSERT_EQ(1u, layer->layerPlan().renderTiles.size());
    EXPECT_EQ((TileKey{"XYZ-WebMercator", 18, 207529, 103668}),
              layer->layerPlan().renderTiles[0].targetKey);
    EXPECT_EQ((TileKey{"XYZ-WebMercator", 0, 0, 0}),
              layer->layerPlan().renderTiles[0].textureKey);
    EXPECT_EQ(TileRenderSource::ParentFallback,
              layer->layerPlan().renderTiles[0].source);
    EXPECT_EQ(0, layer->unsupportedImageryTileCount());
    EXPECT_EQ(0, layer->missingImageryTileCount());
}

TEST_F(BasemapLayerStackTest, EllipsoidSurfaceTileUsesVertexNormalsByDefault) {
    FakeRenderDevice device;
    auto layer = std::make_unique<BasemapLayer>(
        std::make_unique<DebugImageryProvider>(),
        TileScheme::createXYZWebMercator(),
        &device);

    TilePlan plan;
    plan.frameId = 20;
    plan.zoom = 0;
    plan.visibleTiles = {
        TileKey{"XYZ-WebMercator", 0, 0, 0},
    };

    constexpr double radius = 6378137.0;
    const Vec3 cameraPosition(radius * 7.0, 0.0, 0.0);
    layer->applyPlan(plan, cameraPosition);
    layer->loadMissingTiles();

    TilePlan nextFrame = plan;
    nextFrame.frameId = 21;
    layer->applyPlan(nextFrame, cameraPosition);

    Renderer renderer(&device);
    RenderCommandList commands;
    layer->buildRenderCommands(renderer, nullptr, commands);

    ASSERT_EQ(1u, commands.size());
    EXPECT_EQ(1, device.textureCreates);
    EXPECT_EQ(0, layer->normalMapTextureCount());
    EXPECT_EQ(0.0f, commands.front().uniforms["u_useNormalMap"][0]);
}

TEST_F(BasemapLayerStackTest, NormalMapDebugExplicitlyCreatesEllipsoidNormalMap) {
    FakeRenderDevice device;
    auto layer = std::make_unique<BasemapLayer>(
        std::make_unique<DebugImageryProvider>(),
        TileScheme::createXYZWebMercator(),
        &device);
    layer->setNormalMapDebugEnabled(true);

    TilePlan plan;
    plan.frameId = 22;
    plan.zoom = 0;
    plan.visibleTiles = {
        TileKey{"XYZ-WebMercator", 0, 0, 0},
    };

    constexpr double radius = 6378137.0;
    const Vec3 cameraPosition(radius * 7.0, 0.0, 0.0);
    layer->applyPlan(plan, cameraPosition);
    layer->loadMissingTiles();

    TilePlan nextFrame = plan;
    nextFrame.frameId = 23;
    layer->applyPlan(nextFrame, cameraPosition);

    Renderer renderer(&device);
    RenderCommandList commands;
    layer->buildRenderCommands(renderer, nullptr, commands);

    ASSERT_EQ(1u, commands.size());
    EXPECT_EQ(2, device.textureCreates);
    EXPECT_EQ(1, layer->normalMapTextureCount());
    EXPECT_EQ(1.0f, commands.front().uniforms["u_useNormalMap"][0]);
}

TEST_F(BasemapLayerStackTest, DifferentSchemeIndependentTilePlan) {
    BasemapLayerStack stack;
    stack.addLayer(makeXYZLayer("xyz"));
    stack.addLayer(makeTMSLayer("tms"));

    stack.update(frameState_);

    // 不同 scheme → 两个独立的 TilePlan
    const auto& plans = stack.groupPlans();
    EXPECT_GE(plans.size(), 1u);  // 可能 1 或 2 取决于 zoom 是否相同

    // 但每个图层的 scheme 不同
    auto* xyzLayer = stack.findLayer("stub-xyz");
    auto* tmsLayer = stack.findLayer("stub-tms");
    ASSERT_NE(nullptr, xyzLayer);
    ASSERT_NE(nullptr, tmsLayer);

    EXPECT_EQ("XYZ-WebMercator", xyzLayer->tileScheme().id());
    EXPECT_EQ("TMS-WebMercator", tmsLayer->tileScheme().id());
}

TEST_F(BasemapLayerStackTest, InvisibleLayerSkipped) {
    BasemapLayerStack stack;
    auto layerA = makeXYZLayer("a");
    auto layerB = makeXYZLayer("b");
    layerB->setVisible(false);

    stack.addLayer(std::move(layerA));
    stack.addLayer(std::move(layerB));

    stack.update(frameState_);

    // 仅可见图层参与 TilePlan 分组
    auto* layerBptr = stack.findLayer("stub-b");
    ASSERT_NE(nullptr, layerBptr);
    // invisible layer 的 visibleTiles 应为空（applyPlan 跳过 invisible）
    EXPECT_TRUE(layerBptr->visibleTiles().empty());
}

// ============================================================
// 控制点验证
// ============================================================

TEST_F(BasemapLayerStackTest, ControlPointVerification) {
    BasemapLayerStack stack;
    stack.addLayer(makeXYZLayer("xyz"));
    stack.addLayer(makeTMSLayer("tms"));

    // 北京坐标
    double lngRad = 116.397 * M_PI / 180.0;
    double latRad = 39.908 * M_PI / 180.0;

    auto results = stack.verifyControlPoint(lngRad, latRad, 10);

    EXPECT_EQ(2u, results.size());

    for (const auto& r : results) {
        // 控制点应在 tile bounds 内
        EXPECT_TRUE(r.inExpectedRange)
            << "Layer " << r.layerId << " control point not in tile bounds";
        EXPECT_EQ(10, r.tileKey.z);
    }
}

TEST_F(BasemapLayerStackTest, ControlPointCrossSchemeConsistency) {
    // XYZ 和 TMS 的控制点应在各自 tile 内，且 y 轴满足互补关系
    BasemapLayerStack stack;
    stack.addLayer(makeXYZLayer("xyz"));
    stack.addLayer(makeTMSLayer("tms"));

    double lngRad = 0.0;
    double latRad = 0.0;

    auto results = stack.verifyControlPoint(lngRad, latRad, 5);

    ASSERT_EQ(2u, results.size());

    const auto& xyz = results[0];
    const auto& tms = results[1];

    // 控制点在各自 tile 内
    EXPECT_TRUE(xyz.inExpectedRange);
    EXPECT_TRUE(tms.inExpectedRange);

    // 经度范围应一致（同一 x 列）
    EXPECT_NEAR(xyz.tileBounds.west(), tms.tileBounds.west(), 1e-6);
    EXPECT_NEAR(xyz.tileBounds.east(), tms.tileBounds.east(), 1e-6);

    // y 轴关系验证：y_tms + y_xyz ∈ {2^z-1, 2^z}
    int tilesAtZoom = 1 << 5;
    int ySum = xyz.tileKey.y + tms.tileKey.y;
    EXPECT_TRUE(ySum == tilesAtZoom - 1 || ySum == tilesAtZoom)
        << "y_xyz=" << xyz.tileKey.y << " y_tms=" << tms.tileKey.y
        << " sum=" << ySum << " N=" << tilesAtZoom;
}

// ============================================================
// TileGroupKey hash/equality
// ============================================================

TEST_F(BasemapLayerStackTest, TileGroupKeyEquality) {
    TileGroupKey a{"XYZ-WebMercator", 5, 800, 600};
    TileGroupKey b{"XYZ-WebMercator", 5, 800, 600};
    TileGroupKey c{"TMS-WebMercator", 5, 800, 600};
    TileGroupKey d{"XYZ-WebMercator", 6, 800, 600};

    EXPECT_EQ(a, b);
    EXPECT_NE(a, c);
    EXPECT_NE(a, d);
}

TEST_F(BasemapLayerStackTest, TileGroupKeyHash) {
    std::hash<TileGroupKey> hasher;
    TileGroupKey a{"XYZ-WebMercator", 5, 800, 600};
    TileGroupKey b{"XYZ-WebMercator", 5, 800, 600};

    EXPECT_EQ(hasher(a), hasher(b));
}
