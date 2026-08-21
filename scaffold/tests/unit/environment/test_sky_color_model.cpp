#include <gtest/gtest.h>

#include "earth_engine/environment/AtmosphereSkyColorGLSL.h"
#include "earth_engine/environment/SkyColorModel.h"

#include <cmath>
#include <string>

using namespace earth_engine;

// L-P4:computeSkyColor 全函数网兜。
// 债:天空色 GLSL 是运行时字符串,host 不编译,色板全内联字面量 —— 改错只能
// 真机肉眼发现。修:色板/数值参数抽成 SkyColorModel.h C++ 常量(单一事实源),
// GLSL 前缀由常量生成、主体只引用常量名;computeSkyColorCpu() 是同一公式的
// host 镜像。本测试:
//   1. 钉死镜像函数的精确数值锚点(地平线/天顶/太空/日落,spaceFactor 边界)
//   2. 钉死"GLSL 无第二处字面量" —— 常量改了 GLSL 前缀自动跟着变,不存在
//      另一份可漂移的 vec3(0.68, 0.79, 0.86) 之类。

namespace {

using Vec3 = std::array<double, 3>;

double maxAbsDiff(const Vec3& a, const Vec3& b) {
    return std::max({std::fabs(a[0] - b[0]),
                     std::fabs(a[1] - b[1]),
                     std::fabs(a[2] - b[2])});
}

}  // namespace

TEST(SkyColorModelTest, HorizonAtNoonIsExactlyHorizonColor) {
    // rayDir 贴地平线(viewUp=0),太阳在天顶:skyT=0、mu=0、sunLow=0
    // → 输出应精确等于地平线雾白蓝(所有日落项归零)。
    const Vec3 out = computeSkyColorCpu(
        {1.0, 0.0, 0.0},  // rayDir
        {0.0, 1.0, 0.0},  // localUp
        {0.0, 1.0, 0.0},  // sun
        0.0);             // spaceFactor
    EXPECT_LT(maxAbsDiff(out, kSkyHorizonColor), 1e-12);
}

TEST(SkyColorModelTest, ZenithAtNoonIsExactlyZenithColor) {
    const Vec3 out = computeSkyColorCpu(
        {0.0, 1.0, 0.0},  // rayDir == localUp (viewUp=1)
        {0.0, 1.0, 0.0},
        {0.0, 1.0, 0.0},
        0.0);
    EXPECT_LT(maxAbsDiff(out, kSkyZenithColor), 1e-12);
}

TEST(SkyColorModelTest, SpaceFactorOneIsExactlySpaceColor) {
    // 太空(spaceFactor=1):基色整体压向 kSkySpace,所有地面/日落项 ×0。
    const Vec3 out = computeSkyColorCpu(
        {1.0, 0.0, 0.0},
        {0.0, 1.0, 0.0},
        {0.0, 1.0, 0.0},
        1.0);
    EXPECT_LT(maxAbsDiff(out, kSkySpaceColor), 1e-12);
}

TEST(SkyColorModelTest, SunsetHorizonTurnsWarmOrange) {
    // 太阳贴地平线(sunElev=0 → sunLow=1),视线朝太阳(mu=1),viewUp=0。
    // 期望:暖橙主导(r 明显高于 g、b),且远暖于正午地平线。
    const Vec3 sunset = computeSkyColorCpu(
        {1.0, 0.0, 0.0},
        {0.0, 1.0, 0.0},
        {1.0, 0.0, 0.0},
        0.0);
    const Vec3 noon = computeSkyColorCpu(
        {1.0, 0.0, 0.0},
        {0.0, 1.0, 0.0},
        {0.0, 1.0, 0.0},
        0.0);
    EXPECT_GT(sunset[0], sunset[1]);
    EXPECT_GT(sunset[1], sunset[2]);
    EXPECT_GT(sunset[0] - noon[0], 0.1);  // 红色分量显著抬升
    EXPECT_GT(noon[2] - sunset[2], 0.1);  // 蓝色分量被暖橙压暗
}

TEST(SkyColorModelTest, OutputStaysInDisplayRange) {
    // 多方向采样:输出必须落在 [0,1] 显示色范围(tonemap 前)。
    for (double az = 0.0; az < 6.283; az += 0.785) {
        for (double el = -0.5; el <= 1.5; el += 0.5) {
            const Vec3 rayDir = {std::cos(az) * std::cos(el),
                                 std::sin(el),
                                 std::sin(az) * std::cos(el)};
            for (double space : {0.0, 0.5, 1.0}) {
                const Vec3 out = computeSkyColorCpu(
                    rayDir, {0.0, 1.0, 0.0}, {0.0, 1.0, 0.0}, space);
                for (double c : out) {
                    EXPECT_GE(c, 0.0);
                    EXPECT_LE(c, 1.0);
                }
            }
        }
    }
}

TEST(SkyColorModelTest, GlslPreambleIsGeneratedFromConstants) {
    const std::string preamble = skyColorGLSLPreamble();
    // 前缀必须包含格式化后的常量值(改常量 → 文本自动变)。
    EXPECT_NE(std::string::npos, preamble.find("0.68"));
    EXPECT_NE(std::string::npos, preamble.find("kSkyHorizon"));
    EXPECT_NE(std::string::npos, preamble.find("kSkyGradientHi"));
    EXPECT_NE(std::string::npos, preamble.find("kSkyZenithCoolScale"));
}

TEST(SkyColorModelTest, ShaderHasNoSecondSourceOfTruthLiterals) {
    const std::string shader = kSkyColorGLSL();
    // 主体只能引用常量名;旧的内联字面量一旦出现 = 第二处事实源,判 FAIL。
    EXPECT_NE(std::string::npos, shader.find("kSkyHorizon"));
    EXPECT_NE(std::string::npos, shader.find("kSkyWarmSunset"));
    // 前缀由常量生成,字面量只在 preamble 出现一次(合法事实源);computeSkyColor
    // 函数体(从 "vec3 computeSkyColor" 起)不得再出现旧的内联 vec3 字面量。
    const size_t bodyStart = shader.find("vec3 computeSkyColor");
    ASSERT_NE(std::string::npos, bodyStart);
    const std::string body = shader.substr(bodyStart);
    EXPECT_EQ(std::string::npos, body.find("vec3(0.68, 0.79, 0.86)"));
    EXPECT_EQ(std::string::npos, body.find("vec3(0.06, 0.24, 0.55)"));
    EXPECT_EQ(std::string::npos, body.find("vec3(1.00, 0.48, 0.20)"));
    // 暖度曲线走 SunsetWarmthRamp 单一来源,主体里不应再手抄 smoothstep。
    EXPECT_NE(std::string::npos, shader.find("sunsetWarmthRamp(sunElev)"));
}
