#pragma once

#include <cstdio>
#include <string>

namespace earth_engine {

/// 日落暖度膝点曲线(量 A)的**唯一事实源**。
///
/// 历史债(L-P2):同一条 smoothstep 0→0.30 曲线在 CPU(SceneFrameStateBuilder)
/// 与 GLSL(AtmosphereSkyColorGLSL.h)各写一份,靠注释口头约定同步 —— 改一处
/// 漏一处 = 天地暖度错位。本文件把膝点常量收敛为单一来源:
///   - CPU 侧用 sunsetWarmthRampCpu()
///   - GLSL 侧用 sunsetWarmthRampGLSL() 生成函数文本(膝点由常量格式化进
///     字符串,不存在第二处可漂移的字面量)
///
/// ⚠️ 量 B(太阳盘红移/压暗,膝点 0.25,AtmosphereBackgroundPass.cpp)是
/// **独立设计**,勿并入本曲线 —— 太阳盘要在更高太阳角就开始红移。
inline constexpr double kSunsetWarmthKnee = 0.30;

/// CPU 侧:太阳贴地平线(sunElev→0)→1,升到膝点以上 → 0。
inline double sunsetWarmthRampCpu(double sunElev) {
    const double e = sunElev > 0.0 ? sunElev : 0.0;
    double t = e / kSunsetWarmthKnee;
    t = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
    return 1.0 - t * t * (3.0 - 2.0 * t);
}

/// GLSL 侧:与 CPU 同一曲线的函数定义文本。膝点由常量格式化生成 —— 改
/// kSunsetWarmthKnee 一处,CPU 与 GLSL 同时生效,不存在第二份字面量。
inline std::string sunsetWarmthRampGLSL() {
    char kneeBuf[32];
    std::snprintf(kneeBuf, sizeof(kneeBuf), "%.2f",
                  static_cast<double>(kSunsetWarmthKnee));
    return std::string("float sunsetWarmthRamp(float sunElev) {\n"
                       "    float e = max(sunElev, 0.0);\n"
                       "    float t = clamp(e / ") +
           kneeBuf + ", 0.0, 1.0);\n"
           "    return 1.0 - t * t * (3.0 - 2.0 * t);\n"
           "}\n";
}

}  // namespace earth_engine
