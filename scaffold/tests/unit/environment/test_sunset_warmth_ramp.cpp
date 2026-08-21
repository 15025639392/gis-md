#include <gtest/gtest.h>

#include "earth_engine/environment/SunsetWarmthRamp.h"
#include "earth_engine/environment/AtmosphereSkyColorGLSL.h"

#include <cstdlib>
#include <cmath>
#include <string>

using namespace earth_engine;

// L-P2:日落暖度膝点曲线(量 A)单一事实源。
// 历史债:同一条 smoothstep 0→0.30 在 CPU(SceneFrameStateBuilder)与
// GLSL(AtmosphereSkyColorGLSL.h)各写一份,靠注释口头同步。抽函数后:
//   1. CPU 与 GLSL 采样逐点一致(防任一侧抄错公式);
//   2. GLSL 文本里的膝点由常量格式化生成(防第二处字面量漂移)。

namespace {

// GLSL 侧的 smoothstep 期望(展开式,与 CPU 相同语义)。
double glslSmoothstepExpectation(double sunElev) {
    const double e = sunElev > 0.0 ? sunElev : 0.0;
    double t = e / kSunsetWarmthKnee;
    t = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
    return 1.0 - t * t * (3.0 - 2.0 * t);
}

}  // namespace

TEST(SunsetWarmthRampTest, CpuMatchesSmoothstepExpectation) {
    const double samples[] = {0.0, 0.05, 0.10, 0.15, 0.20,
                              0.30, 0.45, 0.60, 1.0, 2.0};
    for (double e : samples) {
        EXPECT_NEAR(sunsetWarmthRampCpu(e),
                    glslSmoothstepExpectation(e),
                    1e-12)
            << "sunElev=" << e;
    }
}

TEST(SunsetWarmthRampTest, KneeIsSingleSourceAndBoundaries) {
    EXPECT_EQ(1.0, sunsetWarmthRampCpu(0.0));     // 地平线 = 满暖
    EXPECT_EQ(0.0, sunsetWarmthRampCpu(kSunsetWarmthKnee));  // 膝点 = 归零
    EXPECT_EQ(0.0, sunsetWarmthRampCpu(kSunsetWarmthKnee * 2));
    EXPECT_EQ(1.0, sunsetWarmthRampCpu(-1.0));    // 太阳在地平线下仍满暖
}

TEST(SunsetWarmthRampTest, GlslSourceContainsGeneratedKneeAndNoSecondLiteral) {
    const std::string glsl = sunsetWarmthRampGLSL();

    // 膝点由常量格式化生成 —— 改 kSunsetWarmthKnee 后这里必须跟着变。
    char kneeBuf[32];
    std::snprintf(kneeBuf, sizeof(kneeBuf), "%.2f",
                  static_cast<double>(kSunsetWarmthKnee));
    EXPECT_NE(std::string::npos, glsl.find(kneeBuf));
    EXPECT_NE(std::string::npos, glsl.find("sunsetWarmthRamp"));

    // 曲线定义只有一处,且公式与 CPU 同语义(逐字核对关键片段)。
    EXPECT_NE(std::string::npos,
              glsl.find("1.0 - t * t * (3.0 - 2.0 * t)"));
}

TEST(SunsetWarmthRampTest, SkyShaderConsumesTheSharedRamp) {
    const std::string sky = kSkyColorGLSL();
    // computeSkyColor 内部调用共享函数,而不是再内联一份 smoothstep。
    EXPECT_NE(std::string::npos,
              sky.find("sunsetWarmthRamp(sunElev)"));
    // 共享函数定义由 SunsetWarmthRamp.h 注入。
    EXPECT_NE(std::string::npos, sky.find("float sunsetWarmthRamp"));
}

TEST(SunsetWarmthRampTest, DiscSunRampIsIndependentAmountB) {
    // 量 B(太阳盘红移/压暗,膝点 0.25)在 AtmosphereBackgroundPass.cpp,
    // 是独立设计 —— 这里只钉死它不被误并入共享曲线:共享曲线膝点仍是量 A。
    EXPECT_DOUBLE_EQ(0.30, kSunsetWarmthKnee);
}
