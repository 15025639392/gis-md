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
