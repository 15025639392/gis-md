#include <gtest/gtest.h>

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
}

TEST(PlatformIntegrationTest,
     SdkFacadeShowsEllipsoidBeforeIonTerrainNegotiationCompletes) {
    MockRenderDevice device;
    MockPlatformBridge bridge;
    Engine engine(&device);
    engine.onSurfaceCreated();
    engine.onSurfaceChanged(800, 600, 1.0f);

    EarthSceneConfig config;
    config.initialCamera = {116.3913, 39.9039, 10000000.0};
    config.terrain.kind = TerrainSourceKind::QuantizedMesh;
    config.terrain.cesiumIonAssetId = 1;
    config.terrain.cesiumIonAccessToken = "long-lived-token";
    config.terrain.maximumZoom = 2;
    RasterOverlaySourceConfig debugOverlay;
    debugOverlay.imageryKind = ImagerySourceKind::Debug;
    debugOverlay.blocksCompleteRenderable = false;
    config.rasterOverlays.push_back(debugOverlay);

    const std::string endpointUrl =
        "https://api.cesium.com/v1/assets/1/endpoint?access_token="
        "long-lived-token";
    const std::string layerJsonUrl =
        "https://terrain.example/layer.json?access_token=temporary-token";
    bridge.setResponse(
        endpointUrl,
        std::vector<uint8_t>{
            '{', '"', 'u', 'r', 'l', '"', ':', '"',
            'h', 't', 't', 'p', 's', ':', '/', '/',
            't', 'e', 'r', 'r', 'a', 'i', 'n', '.',
            'e', 'x', 'a', 'm', 'p', 'l', 'e', '/',
            '"', ',', '"', 'a', 'c', 'c', 'e', 's',
            's', 'T', 'o', 'k', 'e', 'n', '"', ':',
            '"', 't', 'e', 'm', 'p', 'o', 'r', 'a',
            'r', 'y', '-', 't', 'o', 'k', 'e', 'n',
            '"', '}'});
    bridge.setResponse(
        layerJsonUrl,
        std::vector<uint8_t>{
            '{', '"', 't', 'i', 'l', 'e', 's', '"', ':',
            '[', '"', '{', 'z', '}', '/', '{', 'x', '}',
            '/', '{', 'y', '}', '.', 't', 'e', 'r', 'r',
            'a', 'i', 'n', '"', ']', ',', '"', 'p', 'r',
            'o', 'j', 'e', 'c', 't', 'i', 'o', 'n', '"',
            ':', '"', 'E', 'P', 'S', 'G', ':', '4', '3',
            '2', '6', '"', '}'});

    EarthEngineSdkFacade facade(engine, device, bridge);
    facade.installScene(config);
    EXPECT_EQ(1, bridge.requestCount());

    const int buffersBeforeStartupFrames = device.createdBufferCount;
    for (int i = 0; i < 3; ++i) {
        engine.advanceTime(1.0 / 60.0);
        engine.render(0.0);
    }
    EXPECT_GT(device.createdBufferCount, buffersBeforeStartupFrames);

    facade.update();
    facade.update();
    EXPECT_EQ(2, bridge.requestCount());

    const int submitsBeforeReplacementFrames = device.submitCount;
    for (int i = 0; i < 3; ++i) {
        engine.advanceTime(1.0 / 60.0);
        engine.render(0.0);
    }
    EXPECT_GT(device.submitCount, submitsBeforeReplacementFrames);
}

// 验证测试数据 helper
TEST(PlatformIntegrationTest, TestDataHelperCreatesValidQuantizedMesh) {
    auto bytes = makeQuantizedMeshBytes();
    EXPECT_GT(bytes.size(), 92u);
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
