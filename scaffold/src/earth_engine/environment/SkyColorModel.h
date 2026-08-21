#pragma once

#include <array>
#include <cmath>
#include <cstdio>
#include <string>

namespace earth_engine {

/// computeSkyColor(AtmosphereSkyColorGLSL.h)的**数值模型唯一事实源**(L-P4)。
///
/// 历史债:整段天空色 GLSL 是运行时字符串,host ctest 不编译,色板/参数全内联
/// 在字面量里 —— 改错只能真机肉眼发现。本文件把 computeSkyColor 的全部数值
/// 参数抽成 C++ 常量(单一来源):
///   - skyColorGLSLPreamble() 把常量格式化进 GLSL 前缀(供 shader 引用)
///   - computeSkyColorCpu() 是同一公式的 host 可执行镜像(供 ctest 采样对拍)
///
/// ⚠️ 与 SunsetWarmthRamp.h 的关系:暖度曲线(sunLow)不在这里,在
/// SunsetWarmthRamp.h;两处都作为 computeSkyColor 的前缀拼接。
/// 太阳盘/晕/霞光(量 B)在 AtmosphereBackgroundPass.cpp,不属本模型。

// ---- 色板(唯一事实源,勿在 GLSL 主体里另写字面量)----
inline constexpr std::array<double, 3> kSkyHorizonColor = {0.68, 0.79, 0.86};
inline constexpr std::array<double, 3> kSkyZenithColor = {0.06, 0.24, 0.55};
inline constexpr std::array<double, 3> kSkySpaceColor = {0.0, 0.005, 0.025};

// 日间→日落暖色端点(mix 按 sunLow)。
inline constexpr std::array<double, 3> kSkyWarmDayColor = {0.95, 0.90, 0.82};
inline constexpr std::array<double, 3> kSkyWarmSunsetColor = {1.00, 0.48, 0.20};

// 日落整条地平线弱暖洗 + 天顶冷蓝紫(两处末层敷色)。
inline constexpr std::array<double, 3> kSkyWashSunsetColor = {0.80, 0.52, 0.45};
inline constexpr std::array<double, 3> kSkyZenithCoolColor = {0.10, 0.16, 0.42};

// ---- 数值参数(唯一事实源)----
inline constexpr double kSkyGradientLo = 0.0;    // smoothstep 起点(仰角 0)
inline constexpr double kSkyGradientHi = 0.85;   // smoothstep 终点
inline constexpr double kSkyGradientPow = 0.85;  // skyT 幂次(压向天顶蓝)
inline constexpr double kSkyLowSkyPow = 2.5;     // lowSky 收紧幂次
inline constexpr double kSkyGlowTightPow = 8.0;  // 朝阳前向辉光(高日光)
inline constexpr double kSkyGlowTightScale = 0.30;
inline constexpr double kSkyGlowWidePow = 3.0;   // 日落展宽辉光
inline constexpr double kSkyGlowWideScale = 0.60;
inline constexpr double kSkyGlowMixClamp = 0.92; // glow 层 mix 上限
inline constexpr double kSkyWashScale = 0.20;    // 整条地平线暖洗强度
inline constexpr double kSkyZenithCoolScale = 0.25;  // 天顶冷紫强度

/// 生成 GLSL 常量前缀(与 computeSkyColorCpu 同一组常量)。
inline std::string skyColorGLSLPreamble() {
    auto vec3 = [](const std::array<double, 3>& c, const char* name) {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "const vec3 %s = vec3(%.3f, %.3f, %.3f);\n",
                      name, c[0], c[1], c[2]);
        return std::string(buf);
    };
    auto f = [](double v, const char* name) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "const float %s = %.3f;\n", name, v);
        return std::string(buf);
    };
    return vec3(kSkyHorizonColor, "kSkyHorizon") +
           vec3(kSkyZenithColor, "kSkyZenith") +
           vec3(kSkySpaceColor, "kSkySpace") +
           vec3(kSkyWarmDayColor, "kSkyWarmDay") +
           vec3(kSkyWarmSunsetColor, "kSkyWarmSunset") +
           vec3(kSkyWashSunsetColor, "kSkyWashSunset") +
           vec3(kSkyZenithCoolColor, "kSkyZenithCool") +
           f(kSkyGradientLo, "kSkyGradientLo") +
           f(kSkyGradientHi, "kSkyGradientHi") +
           f(kSkyGradientPow, "kSkyGradientPow") +
           f(kSkyLowSkyPow, "kSkyLowSkyPow") +
           f(kSkyGlowTightPow, "kSkyGlowTightPow") +
           f(kSkyGlowTightScale, "kSkyGlowTightScale") +
           f(kSkyGlowWidePow, "kSkyGlowWidePow") +
           f(kSkyGlowWideScale, "kSkyGlowWideScale") +
           f(kSkyGlowMixClamp, "kSkyGlowMixClamp") +
           f(kSkyWashScale, "kSkyWashScale") +
           f(kSkyZenithCoolScale, "kSkyZenithCoolScale");
}

/// 镜像 computeSkyColor(host 可执行)。入参为 ECEF 单位向量:
/// rayDir=视线方向, localUp=当地天顶, sun=太阳方向, spaceFactor=0(近地)→1(太空)。
/// 输出 = 显示色 RGB(与 GLSL 同一公式,逐行对应)。
inline std::array<double, 3> computeSkyColorCpu(
    const std::array<double, 3>& rayDir,
    const std::array<double, 3>& localUp,
    const std::array<double, 3>& sun,
    double spaceFactor) {
    auto dot = [](const std::array<double, 3>& a,
                  const std::array<double, 3>& b) {
        return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    };
    auto mix = [](const std::array<double, 3>& a,
                  const std::array<double, 3>& b, double t) {
        return std::array<double, 3>{a[0] + (b[0] - a[0]) * t,
                                     a[1] + (b[1] - a[1]) * t,
                                     a[2] + (b[2] - a[2]) * t};
    };
    auto clamp01 = [](double x) { return x < 0.0 ? 0.0 : (x > 1.0 ? 1.0 : x); };

    const double viewUp = dot(rayDir, localUp);
    // smoothstep(kSkyGradientLo, kSkyGradientHi, viewUp)
    double skyT = clamp01((viewUp - kSkyGradientLo) /
                          (kSkyGradientHi - kSkyGradientLo));
    skyT = skyT * skyT * (3.0 - 2.0 * skyT);
    std::array<double, 3> sky =
        mix(kSkyHorizonColor, kSkyZenithColor, std::pow(skyT, kSkyGradientPow));
    sky = mix(sky, kSkySpaceColor, spaceFactor);

    const double mu = std::max(dot(rayDir, sun), 0.0);
    const double sunElev = dot(sun, localUp);
    // 暖度曲线在 SunsetWarmthRamp.h(单一来源,此处镜像其数值语义)。
    const double e = sunElev > 0.0 ? sunElev : 0.0;
    double tt = e / 0.30;
    tt = tt < 0.0 ? 0.0 : (tt > 1.0 ? 1.0 : tt);
    const double sunLow = 1.0 - tt * tt * (3.0 - 2.0 * tt);

    const double lowSky = std::pow(1.0 - skyT, kSkyLowSkyPow);
    const std::array<double, 3> warm =
        mix(kSkyWarmDayColor, kSkyWarmSunsetColor, sunLow);
    const double glow = std::pow(mu, kSkyGlowTightPow) * kSkyGlowTightScale +
                        std::pow(mu, kSkyGlowWidePow) * kSkyGlowWideScale *
                            sunLow;
    const double glowMix =
        std::clamp(glow * lowSky * (1.0 - spaceFactor), 0.0, kSkyGlowMixClamp);
    sky = mix(sky, warm, glowMix);
    sky = mix(sky, kSkyWashSunsetColor,
              sunLow * lowSky * kSkyWashScale * (1.0 - spaceFactor));
    sky = mix(sky, kSkyZenithCoolColor,
              sunLow * skyT * kSkyZenithCoolScale * (1.0 - spaceFactor));
    return sky;
}

}  // namespace earth_engine
