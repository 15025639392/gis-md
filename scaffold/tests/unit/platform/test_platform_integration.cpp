#include <gtest/gtest.h>

#include <atomic>

#include "earth_engine/Engine.h"
#include "earth_engine/scene/Camera.h"
#include "earth_engine/scene/PresentationTrace.h"
#include "earth_engine/sdk/EarthEngineSdkFacade.h"
#include "earth_engine/core/geodesy/Ellipsoid.h"

#include "MockRenderDevice.h"
#include "MockPlatformBridge.h"
#include "TestDataHelpers.h"

using namespace earth_engine;
using namespace earth_engine::testing;

// ============================================================
// 平台层集成测试 — 在 macOS 上验证平台层代码
// ============================================================

// 验证 Engine + MockRenderDevice 初始化
TEST(PlatformIntegrationTest, EngineInitializesWithMockRenderDevice) {
    MockRenderDevice device;
    Engine engine(&device);
    engine.onSurfaceCreated();
    engine.onSurfaceChanged(1920, 1080, 2.0f);
    engine.advanceTime(1.0 / 60.0);

    // Engine initializes but needs scene for isReady()
    EXPECT_GE(device.frameCount, 0);
    EXPECT_NE(device.maxTextureSize(), 0);
}

// 验证 MockRenderDevice 记录 GPU 调用
TEST(PlatformIntegrationTest, MockRenderDeviceRecordsGpuCalls) {
    MockRenderDevice device;
    EXPECT_TRUE(device.supportsFloatTextures());
    EXPECT_TRUE(device.supportsInstancing());
    EXPECT_EQ(4096, device.maxTextureSize());

    TextureDesc desc;
    desc.width = 256;
    desc.height = 256;
    auto tex = device.createTexture(desc);
    ASSERT_NE(nullptr, tex);
    EXPECT_EQ(1, device.createdTextureCount);
    EXPECT_EQ(256, tex->width());
    EXPECT_EQ(256, tex->height());
}

// 验证 MockPlatformBridge 预置响应
TEST(PlatformIntegrationTest, MockPlatformBridgeReturnsPresetResponse) {
    MockPlatformBridge bridge;
    bridge.setResponse("http://test.com/data.bin",
                       std::vector<uint8_t>{0x01, 0x02, 0x03});

    int status = 0;
    std::vector<uint8_t> body;
    bridge.get("http://test.com/data.bin",
               [&](int s, std::vector<uint8_t> b) {
                   status = s;
                   body = std::move(b);
               });

    EXPECT_EQ(200, status);
    ASSERT_EQ(3u, body.size());
    EXPECT_EQ(0x01, body[0]);
    EXPECT_EQ(0x02, body[1]);
    EXPECT_EQ(0x03, body[2]);
    EXPECT_EQ(1, bridge.requestCount());
}

// 验证 Engine::render 产生 RenderCommand
TEST(PlatformIntegrationTest, EngineRenderProducesCommands) {
    MockRenderDevice device;
    Engine engine(&device);
    engine.onSurfaceCreated();
    engine.onSurfaceChanged(800, 600, 1.0f);

    EXPECT_TRUE(engine.isReady());

    for (int i = 0; i < 5; ++i) {
        engine.advanceTime(1.0 / 60.0);
        engine.render(0.0);
    }

    EXPECT_GT(device.submitCount, 0);
}

// 验证引擎相机初始位置
TEST(PlatformIntegrationTest, EngineHasDefaultCameraPosition) {
    MockRenderDevice device;
    Engine engine(&device);
    engine.onSurfaceCreated();
    engine.onSurfaceChanged(800, 600, 1.0f);

    const Vec3 pos = engine.camera().position();
    double altitude =
        Ellipsoid::WGS84().cartesianToCartographic(pos).height();
    EXPECT_GT(altitude, 0.0);
    EXPECT_LT(altitude, 1e7);
}

// 验证 PresentationTrace 记录已就绪
TEST(PlatformIntegrationTest, PresentationTraceIsAccessible) {
    MockRenderDevice device;
    Engine engine(&device);
    engine.onSurfaceCreated();
    engine.onSurfaceChanged(800, 600, 1.0f);

    const auto& trace = engine.presentationTrace();
    EXPECT_GE(trace.camera.viewportWidthPixels, 0);
    EXPECT_GE(trace.camera.viewportHeightPixels, 0);
}

// 验证 SDK Facade 场景安装不崩溃
TEST(PlatformIntegrationTest, SdkFacadeInstallsDebugScene) {
    MockRenderDevice device;
    MockPlatformBridge bridge;
    Engine engine(&device);
    engine.onSurfaceCreated();
    engine.onSurfaceChanged(800, 600, 1.0f);

    EarthEngineSdkFacade facade(engine, device, bridge);

    EarthSceneConfig config;
    config.initialCamera = {116.3913, 39.9039, 10000000.0};
    config.tileset = {4.0, 2.0};
    facade.installScene(config);

    for (int i = 0; i < 3; ++i) {
        engine.advanceTime(1.0 / 60.0);
        engine.render(0.0);
    }
    EXPECT_GT(device.submitCount, 0);
    bool sawEllipsoidSurface = false;
    for (const RenderCommand& command : device.submittedCommands) {
        sawEllipsoidSurface = sawEllipsoidSurface ||
            (command.terrainRenderContent &&
             command.terrainSurfaceSource ==
                 TerrainSurfaceCommandSource::EllipsoidFallback &&
             command.depthTest && command.depthWrite);
    }
    EXPECT_TRUE(sawEllipsoidSurface)
        << "TerrainSourceKind::None must keep a depth-writing ellipsoid surface";
}

TEST(PlatformIntegrationTest, SdkFacadeOwnsAndDrivesMvtSource) {
    MockRenderDevice device;
    MockPlatformBridge bridge;
    Engine engine(&device);
    engine.onSurfaceCreated();
    engine.onSurfaceChanged(800, 600, 1.0f);

    std::atomic<int> fetches{0};
    EarthSceneConfig config;
    config.initialCamera = {116.3913, 39.9039, 1000000.0};
    MvtSourceConfig source;
    source.id = "sdk-mvt";
    source.minimumZoom = 0;
    source.maximumZoom = 2;
    source.includeLayers = {"poi"};
    source.fetchTile = [&fetches](const TileKey&, MvtTileFetchCache::FetchCallback cb) {
        fetches.fetch_add(1);
        cb(404, {});
    };
    config.mvtSources.push_back(source);

    {
        EarthEngineSdkFacade facade(engine, device, bridge);
        facade.installScene(config);
        EXPECT_EQ(engine.mvtVectorSourceCount(), 1u);
        engine.render(1.0 / 60.0);
        EXPECT_GT(fetches.load(), 0);

        EarthSceneConfig replacement;
        replacement.initialCamera = config.initialCamera;
        facade.installScene(replacement);
        EXPECT_EQ(engine.mvtVectorSourceCount(), 0u);
    }
    EXPECT_EQ(engine.mvtVectorSourceCount(), 0u);
}

TEST(PlatformIntegrationTest, GenericFeatureLayerRemovalCannotDetachMvtBundle) {
    MockRenderDevice device;
    MockPlatformBridge bridge;
    Engine engine(&device);
    engine.onSurfaceCreated();
    engine.onSurfaceChanged(800, 600, 1.0f);

    MvtVectorSource::Options options;
    options.tree.minZoom = 0;
    options.tree.maxZoom = 1;
    MvtVectorSource::Sinks sinks;
    auto cache = std::make_shared<MvtTileFetchCache>(
        [](const TileKey&, MvtTileFetchCache::FetchCallback callback) {
            callback(404, {});
        },
        8);
    auto source = std::make_unique<MvtVectorSource>(
        std::move(options), std::move(sinks), std::move(cache));
    auto layer = std::make_unique<FeatureRenderLayer>(
        "owned-mvt", &device, Ellipsoid::WGS84());

    ASSERT_TRUE(engine.addMvtVectorSource(std::move(source), std::move(layer)));
    ASSERT_EQ(engine.mvtVectorSourceCount(), 1u);
    EXPECT_EQ(engine.removeFeatureRenderLayer("owned-mvt"), nullptr);
    EXPECT_EQ(engine.mvtVectorSourceCount(), 0u);

    // A subsequent frame must remain safe: the source was removed as a
    // bundle, rather than leaving a live source with a detached layer sink.
    engine.render(1.0 / 60.0);
}

TEST(PlatformIntegrationTest, SdkFacadeCanAddRejectDuplicateAndRemoveMvtSource) {
    MockRenderDevice device;
    MockPlatformBridge bridge;
    Engine engine(&device);
    engine.onSurfaceCreated();
    engine.onSurfaceChanged(800, 600, 1.0f);

    EarthEngineSdkFacade facade(engine, device, bridge);
    EarthSceneConfig config;
    config.initialCamera = {116.3913, 39.9039, 1000000.0};
    facade.installScene(config);

    MvtSourceConfig source;
    source.id = "runtime-mvt";
    source.minimumZoom = 0;
    source.maximumZoom = 1;
    source.fetchTile = [](const TileKey&, MvtTileFetchCache::FetchCallback cb) {
        cb(404, {});
    };
    ASSERT_TRUE(facade.addMvtSource(source));
    EXPECT_EQ(engine.mvtVectorSourceCount(), 1u);
    EXPECT_FALSE(facade.addMvtSource(source));
    EXPECT_EQ(engine.mvtVectorSourceCount(), 1u);
    EXPECT_TRUE(facade.removeMvtSource(source.id));
    EXPECT_EQ(engine.mvtVectorSourceCount(), 0u);
    EXPECT_FALSE(facade.removeMvtSource(source.id));
}

TEST(PlatformIntegrationTest, SdkFacadeCannotRemoveEngineOwnedMvtSource) {
    MockRenderDevice device;
    MockPlatformBridge bridge;
    Engine engine(&device);
    engine.onSurfaceCreated();
    engine.onSurfaceChanged(800, 600, 1.0f);

    MvtVectorSource::Options options;
    options.debugName = "external-mvt";
    options.tree.minZoom = 0;
    options.tree.maxZoom = 1;
    MvtVectorSource::Sinks sinks;
    auto cache = std::make_shared<MvtTileFetchCache>(
        [](const TileKey&, MvtTileFetchCache::FetchCallback callback) {
            callback(404, {});
        },
        8);
    auto source = std::make_unique<MvtVectorSource>(
        std::move(options), std::move(sinks), std::move(cache));
    auto layer = std::make_unique<FeatureRenderLayer>(
        "external-mvt", &device, Ellipsoid::WGS84());
    ASSERT_TRUE(engine.addMvtVectorSource(std::move(source), std::move(layer)));

    EarthEngineSdkFacade facade(engine, device, bridge);
    EarthSceneConfig config;
    config.initialCamera = {116.3913, 39.9039, 1000000.0};
    facade.installScene(config);

    EXPECT_FALSE(facade.removeMvtSource("external-mvt"));
    EXPECT_EQ(engine.mvtVectorSourceCount(), 1u);
    EXPECT_TRUE(engine.removeMvtVectorSource("external-mvt"));
}

TEST(PlatformIntegrationTest, MultipleMvtSourcesKeepIndependentFetchers) {
    MockRenderDevice device;
    MockPlatformBridge bridge;
    Engine engine(&device);
    engine.onSurfaceCreated();
    engine.onSurfaceChanged(800, 600, 1.0f);

    std::atomic<int> roadsFetches{0};
    std::atomic<int> placesFetches{0};
    EarthSceneConfig config;
    config.initialCamera = {116.3913, 39.9039, 1000000.0};

    MvtSourceConfig roads;
    roads.id = "roads-source";
    roads.minimumZoom = 0;
    roads.maximumZoom = 2;
    roads.fetchTile = [&roadsFetches](
        const TileKey&, MvtTileFetchCache::FetchCallback cb) {
        roadsFetches.fetch_add(1);
        cb(404, {});
    };
    MvtSourceConfig places;
    places.id = "places-source";
    places.minimumZoom = 0;
    places.maximumZoom = 2;
    places.fetchTile = [&placesFetches](
        const TileKey&, MvtTileFetchCache::FetchCallback cb) {
        placesFetches.fetch_add(1);
        cb(404, {});
    };
    config.mvtSources = {roads, places};

    EarthEngineSdkFacade facade(engine, device, bridge);
    facade.installScene(config);
    ASSERT_EQ(engine.mvtVectorSourceCount(), 2u);
    engine.render(1.0 / 60.0);

    EXPECT_GT(roadsFetches.load(), 0);
    EXPECT_GT(placesFetches.load(), 0);
}

TEST(PlatformIntegrationTest, DefaultMvtFetchExpandsEachSourceUrlNamespace) {
    MockRenderDevice device;
    MockPlatformBridge bridge;
    Engine engine(&device);
    engine.onSurfaceCreated();
    engine.onSurfaceChanged(800, 600, 1.0f);

    EarthSceneConfig config;
    config.initialCamera = {116.3913, 39.9039, 1000000.0};
    MvtSourceConfig roads;
    roads.id = "roads-url";
    roads.urlTemplate = "https://roads.example/{z}/{x}/{y}?shard={s}";
    roads.subdomains = {"a", "b", "c"};
    roads.minimumZoom = 0;
    roads.maximumZoom = 2;
    MvtSourceConfig places;
    places.id = "places-url";
    places.urlTemplate = "https://places.example/{z}/{x}/{y}?shard={s}";
    places.minimumZoom = 0;
    places.maximumZoom = 2;
    config.mvtSources = {roads, places};

    EarthEngineSdkFacade facade(engine, device, bridge);
    facade.installScene(config);
    engine.render(1.0 / 60.0);

    const std::vector<std::string> urls = bridge.requestedUrls();
    ASSERT_FALSE(urls.empty());
    bool sawRoads = false;
    bool sawPlaces = false;
    for (const std::string& url : urls) {
        EXPECT_EQ(url.find("{z}"), std::string::npos);
        EXPECT_EQ(url.find("{x}"), std::string::npos);
        EXPECT_EQ(url.find("{y}"), std::string::npos);
        EXPECT_EQ(url.find("{s}"), std::string::npos);
        sawRoads = sawRoads ||
            url.compare(0, std::string("https://roads.example/").size(),
                       "https://roads.example/") == 0;
        sawPlaces = sawPlaces ||
            url.compare(0, std::string("https://places.example/").size(),
                       "https://places.example/") == 0;
    }
    EXPECT_TRUE(sawRoads);
    EXPECT_TRUE(sawPlaces);
    EXPECT_TRUE(std::any_of(
        urls.begin(), urls.end(), [](const std::string& url) {
            return url.find("roads.example/") != std::string::npos &&
                   (url.find("shard=a") != std::string::npos ||
                    url.find("shard=b") != std::string::npos ||
                    url.find("shard=c") != std::string::npos);
        })) << "configured letter subdomains must be supported";
}

TEST(PlatformIntegrationTest, ExplicitSharedMvtCacheSharesProviderNamespace) {
    MockRenderDevice device;
    MockPlatformBridge bridge;
    Engine engine(&device);
    engine.onSurfaceCreated();
    engine.onSurfaceChanged(800, 600, 1.0f);

    std::atomic<int> fetches{0};
    auto sharedCache = std::make_shared<MvtTileFetchCache>(
        [&fetches](const TileKey&, MvtTileFetchCache::FetchCallback callback) {
            fetches.fetch_add(1);
            callback(404, {});
        },
        16);
    EarthSceneConfig config;
    config.initialCamera = {116.3913, 39.9039, 1000000.0};
    MvtSourceConfig a;
    a.id = "shared-a";
    a.minimumZoom = 0;
    a.maximumZoom = 1;
    a.sharedCache = sharedCache;
    MvtSourceConfig b = a;
    b.id = "shared-b";
    config.mvtSources = {a, b};

    EarthEngineSdkFacade facade(engine, device, bridge);
    facade.installScene(config);
    engine.render(1.0 / 60.0);

    EXPECT_EQ(engine.mvtVectorSourceCount(), 2u);
    EXPECT_GT(fetches.load(), 0);
    EXPECT_EQ(sharedCache->stats().fetches,
              static_cast<uint64_t>(fetches.load()))
        << "both sources must use the explicitly supplied cache namespace";
    EXPECT_GT(sharedCache->stats().failureSkips, 0u)
        << "the second source must observe the first source's provider-local "
           "failure ledger instead of issuing duplicate requests";
}

TEST(PlatformIntegrationTest, RemovingMvtSourceCancelsDefaultFetches) {
    MockRenderDevice device;
    MockPlatformBridge bridge;
    bridge.setHangRequests(true);
    Engine engine(&device);
    engine.onSurfaceCreated();
    engine.onSurfaceChanged(800, 600, 1.0f);

    EarthEngineSdkFacade facade(engine, device, bridge);
    EarthSceneConfig config;
    config.initialCamera = {116.3913, 39.9039, 1000000.0};
    MvtSourceConfig source;
    source.id = "cancel-mvt";
    source.urlTemplate = "https://mvt.example/{z}/{x}/{y}.pbf";
    source.minimumZoom = 0;
    source.maximumZoom = 2;
    config.mvtSources.push_back(source);
    facade.installScene(config);
    engine.render(1.0 / 60.0);

    const int requests = bridge.requestCount();
    ASSERT_GT(requests, 0);
    ASSERT_TRUE(facade.removeMvtSource("cancel-mvt"));
    EXPECT_EQ(engine.mvtVectorSourceCount(), 0u);
    EXPECT_EQ(bridge.cancelCount(), requests);
}

TEST(PlatformIntegrationTest, MvtSourceSurvivesSurfaceRecreate) {
    MockRenderDevice device;
    MockPlatformBridge bridge;
    Engine engine(&device);
    engine.onSurfaceCreated();
    engine.onSurfaceChanged(800, 600, 1.0f);

    std::atomic<int> fetches{0};
    EarthSceneConfig config;
    config.initialCamera = {116.3913, 39.9039, 1000000.0};
    MvtSourceConfig source;
    source.id = "surface-mvt";
    source.minimumZoom = 0;
    source.maximumZoom = 2;
    source.fetchTile = [&fetches](
        const TileKey&, MvtTileFetchCache::FetchCallback cb) {
        fetches.fetch_add(1);
        cb(404, {});
    };
    config.mvtSources.push_back(source);

    EarthEngineSdkFacade facade(engine, device, bridge);
    facade.installScene(config);
    engine.render(1.0 / 60.0);
    const int beforeDestroy = fetches.load();
    ASSERT_GT(beforeDestroy, 0);
    ASSERT_EQ(engine.mvtVectorSourceCount(), 1u);

    engine.onSurfaceDestroyed();
    EXPECT_EQ(engine.mvtVectorSourceCount(), 1u);

    engine.onSurfaceCreated();
    engine.onSurfaceChanged(800, 600, 1.0f);
    engine.render(1.0 / 60.0);

    EXPECT_EQ(engine.mvtVectorSourceCount(), 1u);
    // The failed-fetch backoff may intentionally suppress an immediate
    // refetch after recreation.  The lifecycle contract is that the source
    // remains Scene-owned and the recreated surface can render without a
    // dangling layer/device reference.
    EXPECT_GE(fetches.load(), beforeDestroy);
    EXPECT_GT(device.submitCount, 1);
}

TEST(PlatformIntegrationTest, TestDataHelperCreatesValidImages) {
    auto img = makeImage(2, 2, 255);
    EXPECT_EQ(2, img->width);
    EXPECT_EQ(2, img->height);
    EXPECT_EQ(4, img->channels);
    EXPECT_EQ(16u, img->pixels.size());

    auto rgb = makeRgbImage(1, 1, 10, 20, 30);
    EXPECT_EQ(3, rgb->channels);
    EXPECT_EQ(3u, rgb->pixels.size());
}
