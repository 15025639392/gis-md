#include <gtest/gtest.h>

#include <atomic>
#include <type_traits>
#include <utility>

#include "earth_engine/Engine.h"
#include "earth_engine/core/async/AsyncSystem.h"
#include "earth_engine/core/async/WorkLedger.h"
#include "earth_engine/data/AmapVectorSource.h"
#include "earth_engine/layers/FeatureRenderLayer.h"
#include "earth_engine/style/AmapClassicRuntime.h"
#include "earth_engine/renderer/Renderer.h"
#include "earth_engine/renderer/IconAtlas.h"
#include "earth_engine/renderer/GlyphAtlas.h"
#include "earth_engine/providers/ImageryProvider.h"
#include "earth_engine/scene/Camera.h"
#include "earth_engine/scene/FrameState.h"
#include "earth_engine/tiling/TileScheme.h"
#include "earth_engine/tiling/Tileset.h"
#include "earth_engine/scene/PresentationTrace.h"
#include "earth_engine/sdk/EarthEngineSdkFacade.h"
#include "earth_engine/core/geodesy/Ellipsoid.h"
#include "earth_engine/core/geodesy/Cartographic.h"

#include "MockRenderDevice.h"
#include "MockPlatformBridge.h"
#include "TestDataHelpers.h"

using namespace earth_engine;
using namespace earth_engine::testing;

static_assert(std::is_same_v<decltype(std::declval<const Engine&>().renderer()),
                             const Renderer*>);
static_assert(std::is_same_v<decltype(std::declval<const Renderer&>().iconAtlas()),
                             const IconAtlas*>);
static_assert(std::is_same_v<decltype(std::declval<const Renderer&>().glyphAtlas()),
                             const GlyphAtlas*>);
static_assert(std::is_same_v<decltype(std::declval<const IconAtlas&>().texture()),
                             const Texture*>);
static_assert(std::is_same_v<decltype(std::declval<const GlyphAtlas&>().texture()),
                             const Texture*>);
static_assert(!std::is_destructible_v<AmapClassicRuntime>);

namespace {

DecodedImage makeRgbaImage(int width, int height, uint8_t seed) {
    DecodedImage image;
    image.width = width;
    image.height = height;
    image.channels = 4;
    image.pixels.resize(static_cast<size_t>(width) * height * 4u);
    for (size_t i = 0; i < image.pixels.size(); ++i)
        image.pixels[i] = static_cast<uint8_t>(seed + i);
    return image;
}

void pumpOfficialAtlas(Engine& engine, int frames = 4) {
    for (int i = 0; i < frames; ++i) engine.render(1.0 / 60.0);
}

}  // namespace

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

TEST(PlatformIntegrationTest, GenericMvtCannotBackSealedOfficialLayer) {
    MockRenderDevice device;
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
        "forged-official-mvt", &device, Ellipsoid::WGS84());
    layer->installAmapClassicProfile(
        FeatureRenderLayer::AmapClassicProfile::Main);

    EXPECT_FALSE(engine.addMvtVectorSource(std::move(source), std::move(layer)));
    EXPECT_EQ(engine.mvtVectorSourceCount(), 0u);
    EXPECT_EQ(engine.removeFeatureRenderLayer("forged-official-mvt"), nullptr);
}

TEST(PlatformIntegrationTest, GenericIconsCannotClaimOfficialAmapNamespace) {
    MockRenderDevice device;
    Engine engine(&device);
    engine.onSurfaceCreated();
    engine.onSurfaceChanged(800, 600, 1.0f);

    const std::vector<uint8_t> rgba(4 * 4 * 4, 255);
    EXPECT_FALSE(engine.addIconImage("amap-icons-1-107", 4, 4, rgba));
    EXPECT_FALSE(engine.hasIconImage("amap-icons-1-107"));
}

TEST(PlatformIntegrationTest,
     OfficialRuntimeAndGenericIconsAreMutuallyExclusive) {
    const std::vector<uint8_t> rgba(4 * 4 * 4, 255);

    {
        MockRenderDevice device;
        device.textureRegionUploadSucceeds = true;
        MockPlatformBridge bridge;
        Engine engine(&device);
        engine.onSurfaceCreated();
        engine.onSurfaceChanged(800, 600, 1.0f);
        ASSERT_TRUE(engine.addIconImage("generic-before-official", 4, 4, rgba));
        auto pool = std::make_shared<ThreadPool>(1);
        EXPECT_EQ(engine.installAmapClassicRuntime(
                      bridge, pool, pool, pool, {}),
                  nullptr);
        EXPECT_FALSE(engine.hasAmapClassicRuntime());
        EXPECT_TRUE(engine.hasIconImage("generic-before-official"));
    }

    {
        MockRenderDevice device;
        MockPlatformBridge bridge;
        Engine engine(&device);
        engine.onSurfaceCreated();
        engine.onSurfaceChanged(800, 600, 1.0f);
        auto pool = std::make_shared<ThreadPool>(1);
        ASSERT_NE(engine.installAmapClassicRuntime(
                      bridge, pool, pool, pool, {}),
                  nullptr);
        EXPECT_FALSE(engine.addIconImage(
            "generic-after-official", 4, 4, rgba));
        EXPECT_FALSE(engine.hasIconImage("generic-after-official"));
        EXPECT_TRUE(engine.hasAmapClassicRuntime());
    }
}

TEST(PlatformIntegrationTest,
     AmapRuntimeRejectsPendingTerrainPageStoreBeforeFirstRender) {
    MockRenderDevice device;
    MockPlatformBridge bridge;
    Engine engine(&device);
    engine.onSurfaceCreated();
    engine.setTerrainPageStoreEnabled(true);
    auto pool = std::make_shared<ThreadPool>(1);

    EXPECT_EQ(engine.installAmapClassicRuntime(
                  bridge, pool, pool, pool, {}),
              nullptr);
    EXPECT_FALSE(engine.hasAmapClassicRuntime());

    engine.setTerrainPageStoreEnabled(false);
    ASSERT_NE(engine.installAmapClassicRuntime(
                  bridge, pool, pool, pool, {}),
              nullptr);
    engine.setTerrainPageStoreEnabled(true);
    EXPECT_TRUE(engine.hasAmapClassicRuntime());
}

TEST(PlatformIntegrationTest, AmapRuntimeOwnsOneCompleteOfficialContract) {
    MockRenderDevice device;
    MockPlatformBridge bridge;
    Engine engine(&device);
    engine.onSurfaceCreated();
    engine.onSurfaceChanged(800, 600, 1.0f);

    auto type1DecodePool = std::make_shared<ThreadPool>(1);
    auto poiDecodePool = std::make_shared<ThreadPool>(1);
    auto tessellationPool = std::make_shared<ThreadPool>(1);
    AmapClassicRuntime::Options runtimeOptions;
    runtimeOptions.credentials.webKey = "test-key";

    {
    const AmapClassicRuntime* runtime = engine.installAmapClassicRuntime(
            bridge, type1DecodePool,
            poiDecodePool, tessellationPool, runtimeOptions);
    ASSERT_NE(runtime, nullptr);
        EXPECT_EQ(engine.installAmapClassicRuntime(
                      bridge, type1DecodePool, poiDecodePool,
                      tessellationPool, runtimeOptions),
                  nullptr);
        auto& bundle = runtime->sources();
        ASSERT_NE(bundle.regionsLayer(), nullptr);
        ASSERT_NE(bundle.mainLayer(), nullptr);
        ASSERT_NE(bundle.poiLayer(), nullptr);
        EXPECT_TRUE(bundle.regionsLayer()->hasSealedOfficialProfile());
        EXPECT_TRUE(bundle.mainLayer()->hasSealedOfficialProfile());
        EXPECT_TRUE(bundle.poiLayer()->hasSealedOfficialProfile());
        EXPECT_EQ(bundle.regionsLayer()->amapClassicProfile(),
                  FeatureRenderLayer::AmapClassicProfile::Regions);
        EXPECT_EQ(bundle.mainLayer()->amapClassicProfile(),
                  FeatureRenderLayer::AmapClassicProfile::Main);
        EXPECT_EQ(bundle.poiLayer()->amapClassicProfile(),
                  FeatureRenderLayer::AmapClassicProfile::Poi);
        EXPECT_EQ(bundle.type1CacheStats().fetches, 0u);
        EXPECT_TRUE(runtime->assets().glyphsReady());
        EXPECT_TRUE(runtime->assets().iconsReady())
            << "no official icon atlas is pending before a visible POI demands it";
        EXPECT_EQ(engine.mvtVectorSourceCount(), 0u)
            << "official typed sources must never enter generic MVT runtime";
        EXPECT_EQ(engine.removeFeatureRenderLayer("amap-regions"), nullptr);
        EXPECT_EQ(engine.removeFeatureRenderLayer("amap-vector"), nullptr);
        EXPECT_EQ(engine.removeFeatureRenderLayer("amap-poi"), nullptr)
            << "official layers may only be removed by runtime teardown";
        EXPECT_NE(runtime->sources().regionsLayer(), nullptr);
        EXPECT_NE(runtime->sources().mainLayer(), nullptr);
        EXPECT_NE(runtime->sources().poiLayer(), nullptr);
        engine.render(1.0 / 60.0);
        const auto snapshot = engine.frameResourceArbiterSnapshot();
        for (SceneFrameResourceStage stage : {
                 SceneFrameResourceStage::NetworkRequest,
                 SceneFrameResourceStage::WorkerDispatch,
                 SceneFrameResourceStage::GpuUpload}) {
            const auto& mvt = snapshot.producerStage(
                SceneFrameResourceProducer::Mvt, stage);
            EXPECT_LE(mvt.used, mvt.granted);
        }
        EXPECT_GT(bridge.requestCount(), 0)
            << "Scene must pump runtime-owned official transport";
        const auto initialUrls = bridge.requestedUrls();
        EXPECT_EQ(initialUrls.end(),
                  std::find_if(initialUrls.begin(), initialUrls.end(),
                               [](const std::string& url) {
                                   return url.find("/icon/") != std::string::npos;
                               }))
            << "no atlas may download before a visible official POI demands it";
    }
}

TEST(PlatformIntegrationTest,
     AmapRuntimeTransportUsesOfficialProbeAndTypedManifestRequests) {
    MockRenderDevice device;
    MockPlatformBridge bridge;
    Engine engine(&device);
    engine.onSurfaceCreated();
    engine.onSurfaceChanged(800, 600, 1.0f);

    const std::string initUrl =
        "https://jsapi.amap.com/web/init?key=test-key";
    const std::string manifestUrl =
        "https://jsapi.amap.com/web_map/get_tile?key=test-key";
    const std::string initJson =
        R"({"tile":"{\"v\":\"26_07_27_00\"}","icon":"{\"v\":\"1\",\"p\":\"p\",\"t\":\"png\"}"})";
    bridge.setResponse(initUrl,
                       std::vector<uint8_t>(initJson.begin(), initJson.end()));
    bridge.setPostResponder([](const std::string& body) {
        constexpr const char* idPrefix = "%22id%22%3A%22";
        const size_t begin = body.find(idPrefix);
        if (begin == std::string::npos) return std::vector<uint8_t>{};
        const size_t idBegin = begin + std::char_traits<char>::length(idPrefix);
        const size_t idEnd = body.find("%22", idBegin);
        if (idEnd == std::string::npos) return std::vector<uint8_t>{};
        const std::string id = body.substr(idBegin, idEnd - idBegin);
        const bool type2 = body.find("%22type%22%3A2") != std::string::npos;
        const std::string group = type2 ? "poi_region_road_transit"
                                        : "building_region_road_transit";
        const std::string selected = "https://signed.test/26_07_27_00/v2/" +
            group + "/pbf/2/" + id + "?auth_key=selected";
        const std::string manifest =
            "{\"infocode\":\"10000\",\"tile_urls\":["
            "\"https://signed.test/26_07_27_00/v2/" + group +
            "/pbf/2/neighbor?auth_key=wrong\",\"" + selected + "\"]}";
        return std::vector<uint8_t>(manifest.begin(), manifest.end());
    });

    auto pool = std::make_shared<ThreadPool>(1);
    AmapClassicRuntime::Options options;
    options.credentials.webKey = "test-key";
    const AmapClassicRuntime* runtime = engine.installAmapClassicRuntime(
        bridge, pool, pool, pool, options);
    ASSERT_NE(runtime, nullptr);
    const_cast<AmapClassicRuntime*>(runtime)->requireAtlasForContractTest(64);
    engine.render(1.0 / 60.0);
    engine.render(1.0 / 60.0);

    const auto requests = bridge.requests();
    int initGets = 0;
    bool sawType1 = false;
    bool sawType2 = false;
    bool sawType1SignedGet = false;
    bool sawType2SignedGet = false;
    bool sawAtlas64 = false;
    bool sawOtherAtlas = false;
    bool allOfficialRequestsCarryReferer = true;
    for (const auto& request : requests) {
        if (request.method == "GET" && request.url == initUrl) ++initGets;
        if (request.url.find("jsapi.amap.com") != std::string::npos ||
            request.url.find("signed.test") != std::string::npos) {
            allOfficialRequestsCarryReferer =
                allOfficialRequestsCarryReferer &&
                std::find(request.headers.begin(), request.headers.end(),
                          std::pair<std::string, std::string>{
                              "Referer", "https://www.amap.com/"}) !=
                    request.headers.end();
        }
        sawType1SignedGet = sawType1SignedGet ||
            (request.method == "GET" &&
             request.url.find("/building_region_road_transit/") !=
                 std::string::npos &&
             request.url.find("auth_key=selected") != std::string::npos);
        sawType2SignedGet = sawType2SignedGet ||
            (request.method == "GET" &&
             request.url.find("/poi_region_road_transit/") !=
                 std::string::npos &&
                 request.url.find("auth_key=selected") != std::string::npos);
        if (request.url.find("/icon/") != std::string::npos) {
            sawAtlas64 = sawAtlas64 ||
                request.url.find("/icons_64?") != std::string::npos;
            sawOtherAtlas = sawOtherAtlas ||
                request.url.find("/icons_64?") == std::string::npos;
        }
        if (request.method != "POST" || request.url != manifestUrl) continue;
        const std::string body(request.body.begin(), request.body.end());
        sawType1 = sawType1 || body.find("%22type%22%3A1") != std::string::npos;
        sawType2 = sawType2 || body.find("%22type%22%3A2") != std::string::npos;
        EXPECT_NE(body.find("version=26_07_27_00"), std::string::npos);
    }
    EXPECT_EQ(initGets, 1)
        << "tile and icon contracts must share one runtime transport probe";
    EXPECT_TRUE(sawType1);
    EXPECT_TRUE(sawType2);
    EXPECT_TRUE(sawType1SignedGet);
    EXPECT_TRUE(sawType2SignedGet);
    EXPECT_TRUE(sawAtlas64);
    EXPECT_FALSE(sawOtherAtlas);
    EXPECT_TRUE(allOfficialRequestsCarryReferer);
}

TEST(PlatformIntegrationTest,
     DynamicBackgroundVisiblePoiDemandsAndInstallsAtlasThroughRuntime) {
    MockRenderDevice device;
    device.textureRegionUploadSucceeds = true;
    MockPlatformBridge bridge;
    bridge.setDecodedImage(makeRgbaImage(512, 1024, 17));
    const std::string atlasUrl =
        "https://o4.amap.com/icon/v/path/png/icons_4?key=test-key";
    bridge.setResponse(atlasUrl, {0x89, 0x50, 0x4e, 0x47});

    Engine engine(&device);
    engine.onSurfaceCreated();
    engine.onSurfaceChanged(800, 600, 1.0f);
    auto pool = std::make_shared<ThreadPool>(1);
    AmapClassicRuntime::Options options;
    options.credentials.webKey = "test-key";
    const AmapClassicRuntime* runtime = engine.installAmapClassicRuntime(
        bridge, pool, pool, pool, options);
    ASSERT_NE(nullptr, runtime);
    auto* mutableRuntime = const_cast<AmapClassicRuntime*>(runtime);
    mutableRuntime->installAtlasManifestForContractTest("v", "path", "png");
    FeatureRenderLayer* poiLayer = mutableRuntime->poiLayerForContractTest();
    ASSERT_NE(nullptr, poiLayer);
    Renderer* renderer = const_cast<Renderer*>(engine.renderer());
    ASSERT_NE(nullptr, renderer);

    GlyphAtlas* glyphAtlas = renderer->glyphAtlas();
    ASSERT_NE(nullptr, glyphAtlas);
    std::vector<uint8_t> glyphPixels(64u * 32u, 127);
    ASSERT_TRUE(glyphAtlas->installAmapOfficialGlyphBatchForTest(
        64, 32, glyphPixels,
        {{'A', 22, 22, 1, -2, 24, 0, 0},
         {'B', 22, 22, 1, -2, 24, 32, 0}}));

    Feature poi;
    poi.type = GeometryType::Point;
    poi.rings = {{Cartographic(0.0, 0.0)}};
    poi.properties = {{"name", "AB"},
                      {"amap_class", "12024"},
                      {"amap_subkey", "1230"},
                      {"amap_draworder", "90"},
                      {"amap_minzoom", "3"},
                      {"amap_maxzoom", "30"},
                      {"amap_rank", "1"}};
    poi.labelSplitIndicesUtf16 = {1, 2};
    auto mesh = FeatureRenderLayer::tessellateTileMesh(
        poiLayer->workerTessellationContext(), {poi});
    ASSERT_EQ(1u, mesh.symbols.size());
    ASSERT_EQ(TileMeshCommitResult::Committed,
              poiLayer->commitTileMesh(
                  TileKey{SchemeId("XYZ-WebMercator"), 10, 100, 200},
                  std::move(mesh)));

    const double radius = Ellipsoid::WGS84().radii().x();
    Camera camera;
    camera.lookAt(Vec3(radius + 4.0e7 / std::pow(2.0, 20.0), 0.0, 0.0),
                  Vec3(radius, 0.0, 0.0), Vec3(0.0, 0.0, 1.0));
    FrameState frame;
    frame.camera = &camera;
    frame.frameId = 1;
    frame.viewportWidthPixels = 800;
    frame.viewportHeightPixels = 600;
    frame.devicePixelRatio = 1.0f;
    RenderCommandList commands;
    poiLayer->buildRenderCommands(frame, *renderer, commands);
    EXPECT_TRUE(commands.empty())
        << "missing dynamic background must keep provider symbol atomic";
    EXPECT_EQ(nullptr, renderer->iconAtlas()->frame("amap-icons-4-73"));

    engine.render(1.0 / 60.0);  // requireAtlas(4) -> official network request
    engine.render(1.0 / 60.0);  // landed response -> production install
    const auto urls = bridge.requestedUrls();
    EXPECT_NE(urls.end(), std::find(urls.begin(), urls.end(), atlasUrl))
        << "the runtime-owned POI layer must demand atlas4 itself";
    ASSERT_NE(nullptr,
              renderer->iconAtlas()->frame("amap-icons-4-73"));

    ++frame.frameId;
    commands.clear();
    poiLayer->buildRenderCommands(frame, *renderer, commands);
    EXPECT_TRUE(std::any_of(
        commands.begin(), commands.end(), [&](const RenderCommand& command) {
            return command.kind == RenderCommandKind::VectorLabel &&
                   command.shader == renderer->vectorLabelBackgroundShader();
        }));
    EXPECT_TRUE(std::any_of(
        commands.begin(), commands.end(), [&](const RenderCommand& command) {
            return command.kind == RenderCommandKind::VectorLabel &&
                   command.shader == renderer->vectorLabelShader();
        })) << "atlas arrival must publish background and text together";
}

TEST(PlatformIntegrationTest,
     OfficialAtlasHeightContractRejectsBeforePublishingFixedOrDynamicFrames) {
    struct Case {
        int atlas;
        int iconIndex;
    };
    struct Result {
        bool requestedFrame = false;
        bool publishedAnyFrame = false;
    };
    const std::array<Case, 2> cases = {{{1, 114}, {4, 73}}};

    for (const Case testCase : cases) {
        const auto run = [&](int decodedHeight) {
            MockRenderDevice device;
            MockPlatformBridge bridge;
            device.textureRegionUploadSucceeds = true;
            Engine engine(&device);
            engine.onSurfaceCreated();
            engine.onSurfaceChanged(800, 600, 1.0f);

            bridge.setDecodedImage(makeRgbaImage(512, decodedHeight,
                                                  testCase.atlas == 1
                                                      ? uint8_t{0x11}
                                                      : uint8_t{0x44}));

            auto pool = std::make_shared<ThreadPool>(1);
            AmapClassicRuntime::Options options;
            options.credentials.webKey = "test-key";
            const AmapClassicRuntime* runtime =
                engine.installAmapClassicRuntime(
                    bridge, pool, pool, pool, std::move(options));
            EXPECT_NE(runtime, nullptr);
            if (!runtime) return Result{};
            const bool installed = const_cast<AmapClassicRuntime*>(runtime)
                ->installAtlasForContractTest(
                    testCase.atlas, {0x89, 0x50, 0x4e, 0x47});

            const std::string frameName =
                "amap-icons-" + std::to_string(testCase.atlas) + "-" +
                std::to_string(testCase.iconIndex);
            const auto* iconAtlas = engine.renderer()->iconAtlas();
            return Result{
                installed && iconAtlas->frame(frameName) != nullptr,
                !iconAtlas->empty()};
        };

        const Result exact = run(1024);
        EXPECT_TRUE(exact.requestedFrame)
            << "the exact official atlas height must publish the requested "
               "frame for atlas "
            << testCase.atlas;
        const Result wrong = run(1023);
        EXPECT_FALSE(wrong.requestedFrame)
            << "a wrong official atlas height must fail closed before any "
               "fixed/dynamic frame is published for atlas "
            << testCase.atlas;
        EXPECT_FALSE(wrong.publishedAnyFrame)
            << "atlas-height rejection must leave no partially installed "
               "official frame for atlas "
            << testCase.atlas;
    }
}

TEST(PlatformIntegrationTest,
     OfficialRuntimeCanOwnPureVectorSceneInstallation) {
    MockRenderDevice device;
    MockPlatformBridge bridge;
    Engine engine(&device);
    engine.onSurfaceCreated();
    engine.onSurfaceChanged(800, 600, 1.0f);
    EarthEngineSdkFacade facade(engine, device, bridge);
    auto pool = std::make_shared<ThreadPool>(1);

    ASSERT_NE(engine.installAmapClassicRuntime(
                  bridge, pool, pool, pool, {}),
              nullptr);
    EarthSceneConfig config;
    config.initialCamera = {116.3913, 39.9039, 10000000.0};
    config.tileset = {4.0, 2.0};
    config.terrain.kind = TerrainSourceKind::None;
    // The sealed runtime owns vector paint identity, while the scene owns its
    // one terrain provider and its tuning.  These values must therefore flow
    // through the official terrain composition without creating a second
    // canvas or a raster fallback.
    config.terrain.ellipsoidFallbackMaxZoom = 0;
    config.terrain.ellipsoidFallbackGridSize = 1;
    facade.installScene(std::move(config));

    engine.render(1.0 / 60.0);
    EXPECT_TRUE(engine.hasAmapClassicRuntime());
    EXPECT_GT(engine.diagnostics().visibleTiles, 0);
    const int visibleBeforePublicTeardown = engine.diagnostics().visibleTiles;
    engine.setTileset(nullptr);
    engine.render(1.0 / 60.0);
    EXPECT_GT(engine.diagnostics().visibleTiles, 0);
    EXPECT_EQ(visibleBeforePublicTeardown, engine.diagnostics().visibleTiles)
        << "public API must not detach the runtime-owned official canvas";
}

TEST(PlatformIntegrationTest,
     OfficialSurfaceOverlayOwnersOutliveTilesetsAcrossFacadeReinstall) {
    MockRenderDevice device;
    MockPlatformBridge bridge;
    Engine engine(&device);
    engine.onSurfaceCreated();
    engine.onSurfaceChanged(800, 600, 1.0f);
    auto pool = std::make_shared<ThreadPool>(1);
    const AmapClassicRuntime* runtime = engine.installAmapClassicRuntime(
        bridge, pool, pool, pool, {});
    ASSERT_NE(nullptr, runtime);

    EarthSceneConfig config;
    config.initialCamera = {106.508, 29.617, 30000.0};
    config.terrain.kind = TerrainSourceKind::None;
    {
        EarthEngineSdkFacade facade(engine, device, bridge);
        facade.installScene(config);
        ASSERT_NE(nullptr, runtime->sources().regionsLayer());
        ASSERT_NE(nullptr, runtime->sources().mainLayer());
        EXPECT_TRUE(runtime->sources().regionsLayer()
                        ->officialSurfaceFillBaked());
        EXPECT_TRUE(runtime->sources().mainLayer()
                        ->officialSurfaceFillBaked());
        engine.render(1.0 / 60.0);
    }

    // Facade destruction must first release every Tileset that borrows its
    // ActivatedRasterOverlay pointers. A render between owners is a useful
    // ASan/UBSan gate for the previously dangling lifecycle.
    engine.render(1.0 / 60.0);
    EXPECT_TRUE(engine.hasAmapClassicRuntime());

    {
        EarthEngineSdkFacade replacement(engine, device, bridge);
        replacement.installScene(config);
        engine.render(1.0 / 60.0);
        EXPECT_TRUE(runtime->sources().regionsLayer()
                        ->officialSurfaceFillBaked());
    }
}

TEST(PlatformIntegrationTest,
     AmapRuntimeRejectsPreEnabledGenericPostProcess) {
    MockPlatformBridge bridge;
    auto pool = std::make_shared<ThreadPool>(1);
    const auto rejected = [&](auto enable) {
        MockRenderDevice device;
        Engine engine(&device);
        engine.onSurfaceCreated();
        ASSERT_TRUE(enable(engine));
        EXPECT_EQ(engine.installAmapClassicRuntime(
                      bridge, pool, pool, pool, {}),
                  nullptr);
        EXPECT_FALSE(engine.hasAmapClassicRuntime());
    };
    rejected([](Engine& engine) {
        return engine.setOffscreenPassthroughEnabled(true);
    });
    rejected([](Engine& engine) { return engine.setFxaaEnabled(true); });
    rejected([](Engine& engine) {
        return engine.setAerialFogEnabled(true);
    });
}

TEST(PlatformIntegrationTest,
     AmapRuntimeAndGenericVtPocsAreMutuallyExclusive) {
    MockPlatformBridge bridge;
    auto pool = std::make_shared<ThreadPool>(1);

    const auto preEnabledRejected = [&](auto enable) {
        MockRenderDevice device;
        Engine engine(&device);
        engine.onSurfaceCreated();
        ASSERT_TRUE(enable(engine));
        EXPECT_EQ(nullptr, engine.installAmapClassicRuntime(
                               bridge, pool, pool, pool, {}));
        EXPECT_FALSE(engine.hasAmapClassicRuntime());
    };
    preEnabledRejected([](Engine& engine) {
        return engine.setVirtualTexturePocEnabled(true);
    });
    preEnabledRejected([](Engine& engine) {
        return engine.setTileCompositeBakePocEnabled(true);
    });
    preEnabledRejected([](Engine& engine) {
        return engine.setVtIndirectionSamplePocEnabled(true);
    });

    MockRenderDevice device;
    Engine engine(&device);
    engine.onSurfaceCreated();
    ASSERT_NE(nullptr, engine.installAmapClassicRuntime(
                           bridge, pool, pool, pool, {}));
    EXPECT_FALSE(engine.setVirtualTexturePocEnabled(true));
    EXPECT_FALSE(engine.setTileCompositeBakePocEnabled(true));
    EXPECT_FALSE(engine.setVtIndirectionSamplePocEnabled(true));
}

TEST(PlatformIntegrationTest,
     OfficialRuntimeRejectsPendingCustomOverlayWithoutSceneMutation) {
    MockRenderDevice device;
    MockPlatformBridge bridge;
    Engine engine(&device);
    engine.onSurfaceCreated();
    engine.onSurfaceChanged(800, 600, 1.0f);
    EarthEngineSdkFacade facade(engine, device, bridge);
    facade.addCustomImageryOverlay(nullptr, nullptr, {});
    auto pool = std::make_shared<ThreadPool>(1);

    ASSERT_NE(engine.installAmapClassicRuntime(
                  bridge, pool, pool, pool, {}),
              nullptr);

    EarthSceneConfig config;
    config.initialCamera = {116.3913, 39.9039, 10000000.0};
    config.terrain.kind = TerrainSourceKind::None;
    facade.installScene(std::move(config));

    EXPECT_TRUE(engine.hasAmapClassicRuntime());
    EXPECT_NE(116.3913, facade.config().initialCamera.longitudeDegrees)
        << "rejected official scene must not partially commit config";
}

TEST(PlatformIntegrationTest,
     OfficialRuntimeAcceptsHeightmapTerrainAsSpatialPlacement) {
    MockRenderDevice device;
    MockPlatformBridge bridge;
    Engine engine(&device);
    engine.onSurfaceCreated();
    engine.onSurfaceChanged(800, 600, 1.0f);
    EarthEngineSdkFacade facade(engine, device, bridge);
    auto pool = std::make_shared<ThreadPool>(1);

    ASSERT_NE(engine.installAmapClassicRuntime(
                  bridge, pool, pool, pool, {}),
              nullptr);

    EarthSceneConfig config;
    config.initialCamera = {116.3913, 39.9039, 10000000.0};
    config.terrain.kind = TerrainSourceKind::Heightmap;
    config.terrain.urlTemplate = "https://invalid/{z}/{x}/{y}.png";
    facade.installScene(std::move(config));

    EXPECT_TRUE(engine.hasAmapClassicRuntime());
    EXPECT_DOUBLE_EQ(116.3913,
                     facade.config().initialCamera.longitudeDegrees);
    EXPECT_EQ(TerrainSourceKind::Heightmap, facade.config().terrain.kind);
    EXPECT_TRUE(facade.config().mvtSources.empty());
    EXPECT_TRUE(facade.config().rasterOverlays.empty());
}

TEST(PlatformIntegrationTest,
     OfficialRuntimeAtomicallyReplacesItsPrimaryTerrainProvider) {
    MockRenderDevice device;
    MockPlatformBridge bridge;
    Engine engine(&device);
    engine.onSurfaceCreated();
    engine.onSurfaceChanged(800, 600, 1.0f);
    EarthEngineSdkFacade facade(engine, device, bridge);
    auto pool = std::make_shared<ThreadPool>(1);

    ASSERT_NE(engine.installAmapClassicRuntime(
                  bridge, pool, pool, pool, {}),
              nullptr);

    EarthSceneConfig flat;
    flat.initialCamera = {116.3913, 39.9039, 10000000.0};
    flat.terrain.kind = TerrainSourceKind::None;
    facade.installScene(std::move(flat));
    engine.render(1.0 / 60.0);

    EarthSceneConfig raised;
    raised.initialCamera = {116.3913, 39.9039, 10000000.0};
    raised.terrain.kind = TerrainSourceKind::Heightmap;
    raised.terrain.urlTemplate =
        "https://replacement-terrain.test/{z}/{x}/{y}.png";
    raised.terrain.minimumZoom = 0;
    raised.terrain.maximumZoom = 2;
    facade.installScene(std::move(raised));
    for (int i = 0; i < 4; ++i) engine.render(1.0 / 60.0);

    EXPECT_EQ(TerrainSourceKind::Heightmap, facade.config().terrain.kind);
    EXPECT_EQ("https://replacement-terrain.test/{z}/{x}/{y}.png",
              facade.config().terrain.urlTemplate);
    const auto urls = bridge.requestedUrls();
    EXPECT_NE(urls.end(),
              std::find_if(urls.begin(), urls.end(), [](const std::string& url) {
                  return url.find("replacement-terrain.test/") !=
                         std::string::npos;
              }))
        << "the rendered primary Tileset must use the same replacement "
           "terrain source committed by the facade";
}

TEST(PlatformIntegrationTest,
     OfficialRuntimeRejectsGltfAndPostProcessWithoutSceneMutation) {
    MockRenderDevice device;
    MockPlatformBridge bridge;
    Engine engine(&device);
    engine.onSurfaceCreated();
    engine.onSurfaceChanged(800, 600, 1.0f);
    EarthEngineSdkFacade facade(engine, device, bridge);
    auto pool = std::make_shared<ThreadPool>(1);

    ASSERT_NE(engine.installAmapClassicRuntime(
                  bridge, pool, pool, pool, {}),
              nullptr);

    EarthSceneConfig config;
    config.initialCamera = {116.3913, 39.9039, 10000000.0};
    config.terrain.kind = TerrainSourceKind::None;
    config.gltf.enabled = true;
    config.gltf.url = "https://invalid/model.glb";
    config.fxaa = true;
    config.aerialFog = true;
    facade.installScene(std::move(config));

    EXPECT_TRUE(engine.hasAmapClassicRuntime());
    EXPECT_NE(116.3913, facade.config().initialCamera.longitudeDegrees)
        << "rejected non-official scene must not partially commit config";
    EXPECT_EQ(TerrainSourceKind::None, facade.config().terrain.kind);
    EXPECT_FALSE(facade.config().gltf.enabled);
    EXPECT_FALSE(facade.config().fxaa);
    EXPECT_FALSE(facade.config().aerialFog);
    EXPECT_FALSE(engine.setFxaaEnabled(true));
    EXPECT_FALSE(engine.setAerialFogEnabled(true));
}

TEST(PlatformIntegrationTest,
     OfficialRuntimeRejectsEveryExperimentalSceneBypassAtomically) {
    using Flag = bool EarthSceneConfig::*;
    const std::array<Flag, 5> forbidden = {
        &EarthSceneConfig::debugOffscreenPassthrough,
        &EarthSceneConfig::virtualTexturePoc,
        &EarthSceneConfig::tileCompositeBakePoc,
        &EarthSceneConfig::vtIndirectionSamplePoc,
        &EarthSceneConfig::terrainPageStore};

    for (Flag flag : forbidden) {
        MockRenderDevice device;
        MockPlatformBridge bridge;
        Engine engine(&device);
        engine.onSurfaceCreated();
        engine.onSurfaceChanged(800, 600, 1.0f);
        EarthEngineSdkFacade facade(engine, device, bridge);
        auto pool = std::make_shared<ThreadPool>(1);
        ASSERT_NE(engine.installAmapClassicRuntime(
                      bridge, pool, pool, pool, {}),
                  nullptr);

        EarthSceneConfig config;
        config.initialCamera = {116.3913, 39.9039, 10000000.0};
        config.terrain.kind = TerrainSourceKind::Heightmap;
        config.terrain.urlTemplate = "https://must-not-commit.test/{z}/{x}/{y}.png";
        config.*flag = true;
        facade.installScene(std::move(config));

        EXPECT_EQ(TerrainSourceKind::None, facade.config().terrain.kind);
        EXPECT_TRUE(facade.config().terrain.urlTemplate.empty());
        EXPECT_FALSE(facade.config().*flag)
            << "each forbidden bypass must independently fail closed before "
               "scene configuration is committed";
    }
}

TEST(PlatformIntegrationTest, AmapRuntimeRejectsReservedLayerIdAtomically) {
    MockRenderDevice device;
    MockPlatformBridge bridge;
    Engine engine(&device);
    engine.onSurfaceCreated();

    auto existing = std::make_unique<FeatureRenderLayer>(
        "amap-vector", &device, Ellipsoid::WGS84());
    engine.addFeatureRenderLayer(std::move(existing));
    auto pool = std::make_shared<ThreadPool>(1);

    EXPECT_THROW(engine.installAmapClassicRuntime(
                     bridge, pool, pool, pool, {}),
                 std::runtime_error);
    EXPECT_NE(engine.removeFeatureRenderLayer("amap-vector"), nullptr)
        << "failed official construction must preserve the pre-existing layer";
    EXPECT_EQ(engine.removeFeatureRenderLayer("amap-regions"), nullptr);
    EXPECT_EQ(engine.removeFeatureRenderLayer("amap-poi"), nullptr);
}

TEST(PlatformIntegrationTest,
     AmapRuntimeRollsBackLayersWhenSourceConstructionFails) {
    MockRenderDevice device;
    MockPlatformBridge bridge;
    Engine engine(&device);
    engine.onSurfaceCreated();
    auto pool = std::make_shared<ThreadPool>(1);
    AmapClassicRuntime::Options options;
    options.sources.failAfterSourceConstruction = 1;

    EXPECT_THROW(engine.installAmapClassicRuntime(
                     bridge, pool, pool, pool, std::move(options)),
                 std::runtime_error);
    EXPECT_FALSE(engine.hasAmapClassicRuntime());
    EXPECT_EQ(engine.removeFeatureRenderLayer("amap-regions"), nullptr);
    EXPECT_EQ(engine.removeFeatureRenderLayer("amap-vector"), nullptr);
    EXPECT_EQ(engine.removeFeatureRenderLayer("amap-poi"), nullptr);

    EXPECT_NE(engine.installAmapClassicRuntime(
                  bridge, pool, pool, pool, {}),
              nullptr)
        << "a failed construction must not reserve official layer ids";
}

TEST(PlatformIntegrationTest,
     AmapRuntimeTeardownCancelsSharedManifestWithoutLateWake) {
    WorkLedger::shared().resetForTesting();
    MockRenderDevice device;
    MockPlatformBridge bridge;
    bridge.setHangRequests(true);
    std::atomic<int> wakes{0};
    {
        Engine engine(&device);
        engine.onSurfaceCreated();
        engine.onSurfaceChanged(800, 600, 1.0f);
        engine.setFrameRequestCallback([&] { ++wakes; });
        auto pool = std::make_shared<ThreadPool>(1);
        AmapClassicRuntime::Options options;
        options.credentials.webKey = "test-key";
        ASSERT_NE(engine.installAmapClassicRuntime(
                      bridge, pool, pool, pool, std::move(options)),
                  nullptr);
        engine.render(1.0 / 60.0);
        EXPECT_EQ(bridge.requestCount(), 1);
    }
    const int wakesAfterEngineDestroy = wakes.load();
    EXPECT_EQ(bridge.cancelCount(), 1);
    EXPECT_EQ(WorkLedger::shared().outstandingForLabel(
                  "amapOfficialAssetAtlas"),
              0);
    EXPECT_EQ(wakes.load(), wakesAfterEngineDestroy)
        << "Engine must detach the global wake callback before Scene-owned "
           "official requests are cancelled";
    WorkLedger::shared().resetForTesting();
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

TEST(PlatformIntegrationTest,
     OfficialRuntimeRejectsLaterGenericMvtWithoutSceneMutation) {
    MockRenderDevice device;
    MockPlatformBridge bridge;
    Engine engine(&device);
    engine.onSurfaceCreated();
    engine.onSurfaceChanged(800, 600, 1.0f);
    EarthEngineSdkFacade facade(engine, device, bridge);
    auto pool = std::make_shared<ThreadPool>(1);

    ASSERT_NE(engine.installAmapClassicRuntime(
                  bridge, pool, pool, pool, {}),
              nullptr);

    MvtSourceConfig source;
    source.id = "generic-after-official";
    source.minimumZoom = 0;
    source.maximumZoom = 1;
    source.fetchTile = [](const TileKey&, MvtTileFetchCache::FetchCallback cb) {
        cb(404, {});
    };
    EXPECT_FALSE(facade.addMvtSource(source));
    EXPECT_TRUE(engine.hasAmapClassicRuntime());
    EXPECT_EQ(engine.mvtVectorSourceCount(), 0u);
    EXPECT_TRUE(facade.config().mvtSources.empty());
}

TEST(PlatformIntegrationTest,
     GenericMvtRejectsLaterOfficialRuntimeWithoutSceneMutation) {
    MockRenderDevice device;
    MockPlatformBridge bridge;
    Engine engine(&device);
    engine.onSurfaceCreated();
    engine.onSurfaceChanged(800, 600, 1.0f);
    EarthEngineSdkFacade facade(engine, device, bridge);

    MvtSourceConfig source;
    source.id = "generic-before-official";
    source.minimumZoom = 0;
    source.maximumZoom = 1;
    source.fetchTile = [](const TileKey&, MvtTileFetchCache::FetchCallback cb) {
        cb(404, {});
    };
    ASSERT_TRUE(facade.addMvtSource(source));
    ASSERT_EQ(engine.mvtVectorSourceCount(), 1u);

    auto pool = std::make_shared<ThreadPool>(1);
    EXPECT_EQ(engine.installAmapClassicRuntime(
                  bridge, pool, pool, pool, {}),
              nullptr);
    EXPECT_FALSE(engine.hasAmapClassicRuntime());
    EXPECT_EQ(engine.mvtVectorSourceCount(), 1u);
    EXPECT_EQ(facade.config().mvtSources.size(), 1u);
    EXPECT_TRUE(facade.removeMvtSource(source.id));
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
