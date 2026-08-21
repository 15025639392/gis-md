#include <gtest/gtest.h>

#include "earth_engine/renderer/PostProcessModel.h"

#include <array>
#include <cmath>
#include <string>

using namespace earth_engine;

// L-P4 剩余:tonemap 曲线 / fog 剂量数学 / 量 B 太阳盘 ramp 的 host 网兜。
// 债:三处数值在 GLSL 字符串里逐字复制两份(纯 Tonemap vs AerialFogTonemap 的
// pbrNeutralToneMapping;纯 AerialFog vs AerialFogTonemap 的 fog 数学),量 B
// 零测试 —— 改错只能真机肉眼发现。修:PostProcessModel.h 常量单一来源 +
// GLSL 生成函数 + C++ 镜像;本测试钉数值行为与"生成文本无第二处字面量"。

namespace {

using Vec3 = std::array<double, 3>;

}  // namespace

// ---- PBR-Neutral tonemap ----

TEST(PostProcessModelTest, TonemapZeroStaysZero) {
    const Vec3 out = pbrNeutralToneMappingCpu({0.0, 0.0, 0.0});
    EXPECT_NEAR(out[0], 0.0, 1e-12);
    EXPECT_NEAR(out[1], 0.0, 1e-12);
    EXPECT_NEAR(out[2], 0.0, 1e-12);
}

TEST(PostProcessModelTest, TonemapBelowCompressionIsLinear) {
    // peak < startCompression(0.76)→ 只做 offset 减法,不压缩。
    const Vec3 in = {0.3, 0.2, 0.1};
    const Vec3 out = pbrNeutralToneMappingCpu(in);
    const double x = 0.1;
    const double offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
    EXPECT_NEAR(out[0], in[0] - offset, 1e-12);
    EXPECT_NEAR(out[1], in[1] - offset, 1e-12);
    EXPECT_NEAR(out[2], in[2] - offset, 1e-12);
}

TEST(PostProcessModelTest, TonemapHighPeakCompressesMonotonically) {
    // 高 HDR 峰值:映射必须单调不降(色调映射保序),且峰值被压到 1 附近。
    const Vec3 in = {2.0, 1.5, 0.5};
    const Vec3 out = pbrNeutralToneMappingCpu(in);
    EXPECT_GT(out[0], out[1]);
    EXPECT_GT(out[1], out[2]);
    EXPECT_LE(out[0], 1.0 + 1e-9);
    EXPECT_GT(out[0], 0.8);
    // 单调性:同分量更高输入 → 不更低输出。
    const Vec3 out2 = pbrNeutralToneMappingCpu({2.5, 1.5, 0.5});
    EXPECT_GE(out2[0], out[0] - 1e-12);
}

TEST(PostProcessModelTest, TonemapGlslIsGeneratedFromConstants) {
    const std::string glsl = pbrNeutralToneMappingGLSL();
    EXPECT_NE(std::string::npos, glsl.find("pbrNeutralToneMapping"));
    EXPECT_NE(std::string::npos, glsl.find("0.7600"));  // startCompression
    EXPECT_NE(std::string::npos, glsl.find("0.1500"));  // desaturation
}

// ---- Aerial fog 剂量数学 ----

TEST(PostProcessModelTest, FogZeroDistanceIsZero) {
    // d=0 且 startDistance>0 → 无雾。
    EXPECT_DOUBLE_EQ(0.0,
                     aerialFogAmountCpu(0.0, 100.0, 0.001, 1.0, 1000.0));
}

TEST(PostProcessModelTest, FogFarDistanceApproachesOne) {
    // 远距离 + 有密度 → 雾趋于饱和。
    // viewUp=0.1:近水平视线,viewWeight≈0.9,不触发地平线强制全雾(否则
    // 近距离也恒 1,测不出"距离累积"这条路径)。
    const double fog = aerialFogAmountCpu(1e9, 0.0, 0.001, 0.1, 1000.0);
    EXPECT_GT(fog, 0.99);
}

TEST(PostProcessModelTest, FogHorizonForcesFullFog) {
    // 视线近水平(viewUp≈0):即使距离很近也强制全雾(消除地平线硬边)。
    const double fog = aerialFogAmountCpu(1.0, 0.0, 0.0, 0.0, 1000.0);
    EXPECT_DOUBLE_EQ(1.0, fog);
}

TEST(PostProcessModelTest, FogHeightDecayClosesAtMaxHeight) {
    // camHeight ≥ 150km:heightWeight→0 → 无雾。
    const double fogLow =
        aerialFogAmountCpu(1e6, 0.0, 0.001, 0.1, 200000.0);
    const double fogHigh =
        aerialFogAmountCpu(1e6, 0.0, 0.001, 0.1, 10.0);
    EXPECT_LT(fogLow, 0.01);
    EXPECT_GT(fogHigh, fogLow);
}

TEST(PostProcessModelTest, FogMathGlslIsGeneratedAndReusable) {
    const std::string glsl = aerialFogMathGLSL();
    EXPECT_NE(std::string::npos, glsl.find("maxHeight"));
    EXPECT_NE(std::string::npos, glsl.find("heightWeight"));
    EXPECT_NE(std::string::npos, glsl.find("150000.0000"));
    EXPECT_NE(std::string::npos, glsl.find("spaceFactor"));
}

// ---- 量 B:太阳盘 ramp ----

TEST(PostProcessModelTest, SunLowSkyRampBoundaries) {
    EXPECT_DOUBLE_EQ(1.0, sunLowSkyRampCpu(-1.0));   // 地平线下满暖
    EXPECT_DOUBLE_EQ(1.0, sunLowSkyRampCpu(0.0));    // 地平线满暖
    EXPECT_NEAR(0.5, sunLowSkyRampCpu(0.125), 1e-12);  // 膝点半程
    EXPECT_DOUBLE_EQ(0.0, sunLowSkyRampCpu(0.25));  // 膝点归零
    EXPECT_DOUBLE_EQ(0.0, sunLowSkyRampCpu(1.0));   // 高太阳
}

TEST(PostProcessModelTest, SunDiscConstantsAreSingleSource) {
    const std::string constants = sunDiscSunsetConstantsGLSL();
    const std::string ramp = sunLowSkyRampGLSL();
    EXPECT_NE(std::string::npos, constants.find("kSunsetTint"));
    EXPECT_NE(std::string::npos, constants.find("kSunDimLow"));
    EXPECT_NE(std::string::npos, constants.find("kDiscSunsetLow"));
    EXPECT_NE(std::string::npos, ramp.find("0.2500"));  // 量 B 膝点,勿并入量 A
    EXPECT_NE(std::string::npos, ramp.find("sunLowSkyRamp"));
}
