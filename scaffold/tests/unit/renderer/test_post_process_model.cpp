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

// ---- Aerial fog 剂量数学(osgEarth 式光学深度路径积分)----

TEST(PostProcessModelTest, FogZeroDistanceIsZero) {
    // d=0 且 startDistance>0 → 无雾。
    EXPECT_DOUBLE_EQ(0.0,
                     aerialFogAmountCpu(0.0, 100.0, 0.001, 1.0, 1000.0));
}

TEST(PostProcessModelTest, FogFarDistanceApproachesOne) {
    // 远距离 + 有密度 → 光学深度积分趋于饱和。
    // d=1e9 被积分上限(400km)截断,水平略仰视线在近地稠密大气内穿行,
    // τ 已远超饱和所需。
    const double fog = aerialFogAmountCpu(1e9, 0.0, 0.001, 0.1, 1000.0);
    EXPECT_GT(fog, 0.99);
}

TEST(PostProcessModelTest, FogNearHorizontalShortDistanceIsClear) {
    // 回归钉子:近处山坡即使视线水平(viewUp≈0)也几乎无雾 —— 这是 osgEarth
    // 式路径积分与旧"地平线强制"的关键差异(旧实现 d=1m 也强制 100%,产生
    // 横切山腰的白带)。光学深度 = 100m × ρ(1500m) ≈ 83,3e-5 消光 → 雾≈0.25%。
    const double fog = aerialFogAmountCpu(100.0, 0.0, 3.0e-5, 0.0, 1500.0);
    EXPECT_LT(fog, 0.01);
}

TEST(PostProcessModelTest, FogSaturatesTowardHorizon) {
    // 远处贴地平线视线:路径长、密度高 → 光学深度饱和,雾趋近 1,与天空无缝
    // (替代旧强制项达成同样的"地平线融进天空",但按路径而非角度)。
    const double fog = aerialFogAmountCpu(500000.0, 0.0, 3.0e-5, 0.0, 1500.0);
    EXPECT_GT(fog, 0.9);
}

TEST(PostProcessModelTest, FogIncreasesMonotonicallyWithDistance) {
    // 同一视线方向:距离越远光学深度越大 → 雾单调不减。
    const double near = aerialFogAmountCpu(1000.0, 0.0, 3.0e-5, 0.0, 1500.0);
    const double mid = aerialFogAmountCpu(100000.0, 0.0, 3.0e-5, 0.0, 1500.0);
    const double far = aerialFogAmountCpu(400000.0, 0.0, 3.0e-5, 0.0, 1500.0);
    EXPECT_LE(near, mid);
    EXPECT_LE(mid, far);
    EXPECT_GT(far, near);
}

TEST(PostProcessModelTest, FogLookingDownIsSmall) {
    // 俯视近景:视线快速接近地面,路径短 → 雾量小(不是角度窗口,是路径短)。
    // 相机 1000m 俯视 30°,1500m 处地形仍在地面上(高度≈250m)。
    const double fog = aerialFogAmountCpu(1500.0, 0.0, 3.0e-5, -0.5, 1000.0);
    EXPECT_LT(fog, 0.1);
}

TEST(PostProcessModelTest, FogHeightDecayClosesAtMaxHeight) {
    // camHeight ≥ 150km(太空):heightWeight→0 → 无雾。
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
    EXPECT_NE(std::string::npos, glsl.find("scaleHeight"));
    EXPECT_NE(std::string::npos, glsl.find("opticalDepth"));
    EXPECT_NE(std::string::npos, glsl.find("150000.0000"));
    EXPECT_NE(std::string::npos, glsl.find("spaceFactor"));
    EXPECT_NE(std::string::npos, glsl.find("7994.0000"));
    EXPECT_EQ(std::string::npos, glsl.find("smoothstep(0.0000, 0.0600"));
    // 回归钉子:uniform 不能初始化 const(GLSL ES 编译错误会令 fog pass 静默
    // 回落直绘,真机表现为"雾整体消失"而非白带修复)。planetRadius 必须是非
    // const 局部变量,从 u_planetRadius 读取。
    EXPECT_EQ(std::string::npos, glsl.find("const float planetRadius"));
    EXPECT_NE(std::string::npos, glsl.find("float planetRadius = u_planetRadius;"));
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
