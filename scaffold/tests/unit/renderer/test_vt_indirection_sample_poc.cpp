#include <gtest/gtest.h>

#include "earth_engine/renderer/VtIndirectionSamplePoc.h"
#include "../../helpers/MockRenderDevice.h"

using namespace earth_engine;
using earth_engine::testing::MockRenderDevice;

namespace {

VtIndirectionSamplePocConfig cfg() {
    VtIndirectionSamplePocConfig c;
    c.indirectionSize = 32;
    c.atlasSize = 64;
    c.passesPerTick = 3;
    return c;
}

}  // namespace

TEST(VtIndirectionSamplePoc, InitializeBuildsSweepShadersAndTextures) {
    MockRenderDevice device;
    VtIndirectionSamplePoc poc;
    ASSERT_TRUE(poc.initialize(&device, cfg()));
    // baseline(descent 0)+ kVtSweepCount 个深度 shader。
    EXPECT_EQ(device.shaderCount, 1 + kVtSweepCount);
    // 间接纹理 + atlas 两张。
    EXPECT_EQ(device.createdTextureCount, 2);
}

TEST(VtIndirectionSamplePoc, InitFailsOnNullDevice) {
    VtIndirectionSamplePoc poc;
    EXPECT_FALSE(poc.initialize(nullptr, cfg()));
}

TEST(VtIndirectionSamplePoc, EnsureBuildsScreenSizedFbo) {
    MockRenderDevice device;
    VtIndirectionSamplePoc poc;
    ASSERT_TRUE(poc.initialize(&device, cfg()));
    EXPECT_TRUE(poc.ensureResources(1080, 2040));
    EXPECT_TRUE(poc.isReady());
    EXPECT_EQ(device.createdFramebufferCount, 1);
}

TEST(VtIndirectionSamplePoc, EnsureRebuildsOnSizeChange) {
    MockRenderDevice device;
    VtIndirectionSamplePoc poc;
    ASSERT_TRUE(poc.initialize(&device, cfg()));
    ASSERT_TRUE(poc.ensureResources(800, 600));
    // 同尺寸不重建。
    ASSERT_TRUE(poc.ensureResources(800, 600));
    EXPECT_EQ(device.createdFramebufferCount, 1);
    // 变尺寸重建。
    ASSERT_TRUE(poc.ensureResources(1080, 2040));
    EXPECT_EQ(device.createdFramebufferCount, 2);
}

TEST(VtIndirectionSamplePoc, TickRunsBothGroupsAndRecordsStats) {
    MockRenderDevice device;
    VtIndirectionSamplePoc poc;
    ASSERT_TRUE(poc.initialize(&device, cfg()));
    ASSERT_TRUE(poc.ensureResources(640, 480));

    const VtIndirectionSampleFrameStats s = poc.tick();
    EXPECT_TRUE(s.ready);
    EXPECT_EQ(s.passes, 3);
    // 组:syncFloor(0 pass)+ baseline(N)+ kVtSweepCount 个深度组(各 N)。
    EXPECT_EQ(device.beginPassCount, (1 + kVtSweepCount) * 3);
    EXPECT_EQ(device.endPassCount, (1 + kVtSweepCount) * 3);
    EXPECT_EQ(s.fillPixels, 640LL * 480);
    EXPECT_GE(s.baselineMs, 0.0);
    for (int i = 0; i < kVtSweepCount; ++i) {
        EXPECT_GE(s.descentMs[i], 0.0);
    }
}

TEST(VtIndirectionSamplePoc, TickBeforeEnsureIsNoop) {
    MockRenderDevice device;
    VtIndirectionSamplePoc poc;
    ASSERT_TRUE(poc.initialize(&device, cfg()));
    // 未 ensureResources → 无 FBO → tick 空转。
    const VtIndirectionSampleFrameStats s = poc.tick();
    EXPECT_FALSE(s.ready);
    EXPECT_EQ(device.beginPassCount, 0);
}

TEST(VtIndirectionSamplePoc, DisposeClearsReady) {
    MockRenderDevice device;
    VtIndirectionSamplePoc poc;
    ASSERT_TRUE(poc.initialize(&device, cfg()));
    ASSERT_TRUE(poc.ensureResources(320, 240));
    ASSERT_TRUE(poc.isReady());
    poc.dispose();
    EXPECT_FALSE(poc.isReady());
}
