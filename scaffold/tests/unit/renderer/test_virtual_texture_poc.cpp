#include <gtest/gtest.h>

#include "earth_engine/renderer/VirtualTexturePoc.h"
#include "../../helpers/MockRenderDevice.h"

#include <vector>

using namespace earth_engine;
using earth_engine::testing::MockRenderDevice;

namespace {

VirtualTexturePocConfig smallConfig() {
    VirtualTexturePocConfig c;
    c.feedbackDownscale = 4;
    c.atlasColumns = 4;
    c.atlasRows = 4;
    c.pageSizeTexels = 64;
    c.indirectionSize = 32;
    return c;
}

}  // namespace

TEST(VirtualTexturePoc, InitializeAllocatesAtlasAndIndirection) {
    MockRenderDevice device;
    VirtualTexturePoc poc;
    ASSERT_TRUE(poc.initialize(&device, smallConfig()));
    EXPECT_TRUE(poc.isReady());
    // atlas + 间接纹理 = 2 张纹理。
    EXPECT_EQ(device.createdTextureCount, 2);
    // atlas 字节 = 4*64 × 4*64 × 4 = 256*256*4。
    EXPECT_EQ(poc.atlasBytes(), 256LL * 256LL * 4LL);
}

TEST(VirtualTexturePoc, InitializeFailsOnNullDevice) {
    VirtualTexturePoc poc;
    EXPECT_FALSE(poc.initialize(nullptr, smallConfig()));
    EXPECT_FALSE(poc.isReady());
}

TEST(VirtualTexturePoc, InitializeFailsWhenTextureCreationFails) {
    MockRenderDevice device;
    device.allowTextureCreation = false;
    VirtualTexturePoc poc;
    EXPECT_FALSE(poc.initialize(&device, smallConfig()));
    EXPECT_FALSE(poc.isReady());
}

TEST(VirtualTexturePoc, EnsureResourcesBuildsFeedbackFboSubsampled) {
    MockRenderDevice device;
    VirtualTexturePoc poc;
    ASSERT_TRUE(poc.initialize(&device, smallConfig()));
    // 800×600 / downscale4 → feedback 200×150。
    EXPECT_TRUE(poc.ensureResources(800, 600));
    EXPECT_EQ(device.createdFramebufferCount, 1);
    // 同尺寸再调不重建。
    EXPECT_TRUE(poc.ensureResources(800, 600));
    EXPECT_EQ(device.createdFramebufferCount, 1);
    // 尺寸变化 → 重建。
    EXPECT_TRUE(poc.ensureResources(400, 300));
    EXPECT_EQ(device.createdFramebufferCount, 2);
}

TEST(VirtualTexturePoc, EnsureResourcesFailsWhenFramebufferUnavailable) {
    MockRenderDevice device;
    device.allowFramebufferCreation = false;
    VirtualTexturePoc poc;
    ASSERT_TRUE(poc.initialize(&device, smallConfig()));
    EXPECT_FALSE(poc.ensureResources(800, 600));
}

TEST(VirtualTexturePoc, TickRunsFeedbackReadbackChainAndRecordsStats) {
    MockRenderDevice device;
    // 预置 canned feedback:一页 (3,5,6),平铺满整帧 → 解码去重成 1 页。
    const auto rgba = vt_codec::encode(VtPageId{3u, 5u, 6u});
    device.cannedFeedbackPixels.assign(rgba.begin(), rgba.end());

    VirtualTexturePoc poc;
    ASSERT_TRUE(poc.initialize(&device, smallConfig()));
    ASSERT_TRUE(poc.ensureResources(400, 300));

    const VirtualTexturePocFrameStats s = poc.tick();
    EXPECT_TRUE(s.readbackOk);
    EXPECT_EQ(device.readbackCount, 1);
    EXPECT_GE(device.beginPassCount, 1);  // feedback pass 开了
    EXPECT_EQ(s.visiblePages, 1);         // 平铺同页去重
    EXPECT_EQ(s.residentPages, 1);
    EXPECT_EQ(s.newlyResident, 1);
    EXPECT_FALSE(s.thrashed);
    // 计时字段被填(mock 下可能极小但非负)。
    EXPECT_GE(s.readbackMs, 0.0);
    EXPECT_GE(s.feedbackPassMs, 0.0);
    EXPECT_GE(s.updateMs, 0.0);
    // 间接纹理被回传(至少 1 次 updateTextureRegion 由 writeIndirection 触发)。
    // MockRenderDevice::updateTextureRegion 返回 false 不计数,故改验 lastStats。
    EXPECT_EQ(poc.lastStats().visiblePages, 1);
}

TEST(VirtualTexturePoc, TickWithBackgroundFeedbackYieldsNoPages) {
    MockRenderDevice device;
    // 不设 canned → 回读全 0 = 全背景 → 0 可见页(骨架未接 shader 时的真机形态)。
    VirtualTexturePoc poc;
    ASSERT_TRUE(poc.initialize(&device, smallConfig()));
    ASSERT_TRUE(poc.ensureResources(400, 300));

    const VirtualTexturePocFrameStats s = poc.tick();
    EXPECT_TRUE(s.readbackOk);  // 回读成功(读到全 0)
    EXPECT_EQ(s.visiblePages, 0);
    EXPECT_EQ(s.residentPages, 0);
}

TEST(VirtualTexturePoc, TickBeforeEnsureResourcesReturnsEmptyStats) {
    MockRenderDevice device;
    VirtualTexturePoc poc;
    ASSERT_TRUE(poc.initialize(&device, smallConfig()));
    // 没 ensureResources → 无 feedback FBO → tick 返回全 0。
    const VirtualTexturePocFrameStats s = poc.tick();
    EXPECT_FALSE(s.readbackOk);
    EXPECT_EQ(device.readbackCount, 0);
}

TEST(VirtualTexturePoc, AsyncPathUsedWhenDeviceSupportsItAfterLatency) {
    MockRenderDevice device;
    device.supportsAsyncReadback = true;
    const auto rgba = vt_codec::encode(VtPageId{3u, 5u, 6u});
    device.cannedFeedbackPixels.assign(rgba.begin(), rgba.end());

    VirtualTexturePocConfig cfg = smallConfig();
    cfg.asyncReadback = true;
    cfg.readbackLatencyFrames = 2;
    VirtualTexturePoc poc;
    ASSERT_TRUE(poc.initialize(&device, cfg));
    ASSERT_TRUE(poc.ensureResources(400, 300));

    // 前 latency 帧:只 enqueue,还没到 acquire → readbackOk=false,但走的是异步路径。
    VirtualTexturePocFrameStats s = poc.tick();
    EXPECT_TRUE(s.async);
    EXPECT_FALSE(s.readbackOk);
    EXPECT_GE(device.enqueueCount, 1);
    poc.tick();
    // 第 3 帧:队列深度 > latency(2),开始 acquire → 取到像素。
    s = poc.tick();
    EXPECT_TRUE(s.async);
    EXPECT_TRUE(s.readbackOk);
    EXPECT_GE(device.acquireCount, 1);
    EXPECT_EQ(s.visiblePages, 1);
}

TEST(VirtualTexturePoc, FallsBackToSyncWhenAsyncUnsupported) {
    MockRenderDevice device;
    device.supportsAsyncReadback = false;  // Metal/mock 不支持
    VirtualTexturePocConfig cfg = smallConfig();
    cfg.asyncReadback = true;  // 请求异步,但后端不支持 → 回落同步
    VirtualTexturePoc poc;
    ASSERT_TRUE(poc.initialize(&device, cfg));
    ASSERT_TRUE(poc.ensureResources(400, 300));

    const VirtualTexturePocFrameStats s = poc.tick();
    EXPECT_FALSE(s.async);          // 未走异步
    EXPECT_EQ(device.enqueueCount, 0);
    EXPECT_EQ(device.readbackCount, 1);  // 走了同步 readFramebufferPixels
    EXPECT_TRUE(s.readbackOk);
}

TEST(VirtualTexturePoc, AsyncPendingKeepsTicketQueuedNoStall) {
    MockRenderDevice device;
    device.supportsAsyncReadback = true;
    device.asyncAlwaysPending = true;  // acquire 恒未就绪
    VirtualTexturePocConfig cfg = smallConfig();
    cfg.readbackLatencyFrames = 1;
    VirtualTexturePoc poc;
    ASSERT_TRUE(poc.initialize(&device, cfg));
    ASSERT_TRUE(poc.ensureResources(400, 300));

    for (int i = 0; i < 5; ++i) poc.tick();
    const VirtualTexturePocFrameStats s = poc.lastStats();
    EXPECT_TRUE(s.async);
    EXPECT_TRUE(s.readbackPending);   // GPU 未完成 → pending(非 stall)
    EXPECT_FALSE(s.readbackOk);
}

TEST(VirtualTexturePoc, DisposeReleasesResources) {
    MockRenderDevice device;
    VirtualTexturePoc poc;
    ASSERT_TRUE(poc.initialize(&device, smallConfig()));
    ASSERT_TRUE(poc.ensureResources(400, 300));
    poc.dispose();
    EXPECT_FALSE(poc.isReady());
    EXPECT_EQ(poc.atlasBytes(), 0);
}
