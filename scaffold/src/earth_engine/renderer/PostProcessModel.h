#pragma once

#include <array>
#include <cmath>
#include <cstdio>
#include <string>

namespace earth_engine {

/// 后处理链数值模型的**唯一事实源**(L-P4 剩余部分)。
///
/// 债:tonemap 曲线与 fog 数学在 OffscreenPostProcess.cpp 里**逐字复制两份**
/// (kTonemapFragGLSL vs kAerialFogTonemapMain 的 pbrNeutralToneMapping;
/// kAerialFogFragMain vs kAerialFogTonemapMain 的 fog 数学),量 B(太阳盘
/// ramp)零测试 —— 改错只能真机肉眼发现。本文件:
///   - 每个模型一个 GLSL 生成函数(常量格式化进文本,无第二处字面量)
///   - 每个模型一个 C++ 镜像函数(host ctest 采样对拍)
///
/// ⚠️ 与 SkyColorModel.h 的关系:雾色本身 = computeSkyColor(那里已收敛);
/// 本文件只收 fog 的**剂量数学**与 tonemap 曲线,不含色板。

namespace postprocess_detail {

inline double smoothstepCpu(double edge0, double edge1, double x) {
    double t = (x - edge0) / (edge1 - edge0);
    t = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
    return t * t * (3.0 - 2.0 * t);
}

inline std::string formatFloat(double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.4f", v);
    return std::string(buf);
}

}  // namespace postprocess_detail

// ============================================================
// PBR-Neutral tonemap(Khronos / Cesium 默认同款)
// ============================================================
inline constexpr double kTonemapStartCompression = 0.8 - 0.04;
inline constexpr double kTonemapDesaturation = 0.15;
inline constexpr double kTonemapOffsetBelow = 0.08;
inline constexpr double kTonemapOffsetQuadratic = 6.25;
inline constexpr double kTonemapOffsetFlat = 0.04;

/// 生成完整 GLSL 函数(两份 shader 共用的唯一来源)。
inline std::string pbrNeutralToneMappingGLSL() {
    using postprocess_detail::formatFloat;
    return std::string("vec3 pbrNeutralToneMapping(vec3 color) {\n") +
           "    const float startCompression = " +
           formatFloat(kTonemapStartCompression) + ";\n" +
           "    const float desaturation = " +
           formatFloat(kTonemapDesaturation) + ";\n" +
           "    float x = min(color.r, min(color.g, color.b));\n" +
           "    float offset = x < " + formatFloat(kTonemapOffsetBelow) +
           " ? x - " + formatFloat(kTonemapOffsetQuadratic) +
           " * x * x : " + formatFloat(kTonemapOffsetFlat) + ";\n" +
           "    color -= offset;\n" +
           "    float peak = max(color.r, max(color.g, color.b));\n" +
           "    if (peak < startCompression) return color;\n" +
           "    float d = 1.0 - startCompression;\n" +
           "    float newPeak = 1.0 - d * d / (peak + d - startCompression);\n" +
           "    color *= newPeak / peak;\n" +
           "    float g = 1.0 - 1.0 / (desaturation * (peak - newPeak) + 1.0);\n" +
           "    return mix(color, vec3(newPeak), g);\n" +
           "}\n";
}

/// C++ 镜像(host 测试用),入/出均为线性 HDR RGB。
inline std::array<double, 3> pbrNeutralToneMappingCpu(
    const std::array<double, 3>& colorIn) {
    auto clamp01 = [](double x) { return x < 0.0 ? 0.0 : (x > 1.0 ? 1.0 : x); };
    std::array<double, 3> color = colorIn;
    const double x = std::min(color[0], std::min(color[1], color[2]));
    const double offset = x < kTonemapOffsetBelow
        ? x - kTonemapOffsetQuadratic * x * x
        : kTonemapOffsetFlat;
    for (double& c : color) c -= offset;
    const double peak = std::max(color[0], std::max(color[1], color[2]));
    if (peak < kTonemapStartCompression) return color;
    const double d = 1.0 - kTonemapStartCompression;
    const double newPeak =
        1.0 - d * d / (peak + d - kTonemapStartCompression);
    for (double& c : color) c *= newPeak / peak;
    const double g = 1.0 - 1.0 /
        (kTonemapDesaturation * (peak - newPeak) + 1.0);
    std::array<double, 3> out;
    for (size_t i = 0; i < 3; ++i) {
        out[i] = color[i] + (newPeak - color[i]) * clamp01(g);
    }
    return out;
}

// ============================================================
// Aerial fog 剂量数学(osgEarth 式大气光学深度路径积分)
// ============================================================
// 对齐参考:osgEarth SimpleSkyNode 的 O'Neil 散射 —— 雾量由视线在大气中
// 穿过的**光学深度**决定,而不是角度窗口。密度剖面取指数 ρ(h)=exp(-h/H),
// H = Rayleigh scale height(7994m,与 AtmosphereParameters::rayleighScaleHeight
// 同值)。沿视线从 startDistance 到 min(d, 积分上限)做 4 段中点采样,累积
// 光学深度 τ,fog = 1-exp(-τ·density):
//   - 远处/贴地平线视线:路径长、密度高 → 自然饱和,与天空无缝(替代旧的
//     逐像素地平线强制项,不再出现横切山腰的白带);
//   - 近处山坡即使视线水平:路径短 → 雾量小,保持本色;
//   - 俯视近景:路径短 → 几乎无雾;
//   - 相机 >150km(太空):heightWeight→0,雾关闭。
// 雾色仍由 computeSkyColor 提供(与大气 pass 同源,见 OffscreenPostProcess)。
inline constexpr double kFogMaxHeight = 150000.0;
inline constexpr double kFogScaleHeight = 7994.0;  // 与 AtmosphereParameters 同值
inline constexpr double kFogMaxIntegrateDistance = 400000.0;
inline constexpr int kFogSampleCount = 4;
inline constexpr double kFogSpaceFactorLo = 120000.0;
inline constexpr double kFogSpaceFactorHi = 900000.0;

/// 生成 GLSL 代码块:声明并计算 heightWeight/viewUp/spaceFactor/opticalDepth/
/// fog(两份 main 共用的唯一来源)。
inline std::string aerialFogMathGLSL() {
    using postprocess_detail::formatFloat;
    std::string s = "        const float maxHeight = " +
                    formatFloat(kFogMaxHeight) + ";\n";
    s += "        const float scaleHeight = " +
         formatFloat(kFogScaleHeight) + ";\n";
    s += "        const float maxIntegrate = " +
         formatFloat(kFogMaxIntegrateDistance) + ";\n";
    s += "        const int sampleCount = " +
         std::to_string(kFogSampleCount) + ";\n";
    s += "        float planetRadius = u_planetRadius;\n";
    s += "        float heightWeight = smoothstep(maxHeight, 0.0, camHeight);\n";
    s += "        float viewUp = dot(rayDir, up);\n";
    s += "        float spaceFactor = smoothstep(" +
         formatFloat(kFogSpaceFactorLo) + ", " +
         formatFloat(kFogSpaceFactorHi) + ", camHeight);\n";
    s += "        float segStart = max(u_fogParams.y, 0.0);\n";
    s += "        float segEnd = min(d, maxIntegrate);\n";
    s += "        float opticalDepth = 0.0;\n";
    s += "        if (segEnd > segStart) {\n";
    s += "            float stepLen = (segEnd - segStart) / float(sampleCount);\n";
    s += "            float Rc = planetRadius + camHeight;\n";
    s += "            for (int i = 0; i < sampleCount; ++i) {\n";
    s += "                float s = segStart + stepLen * (float(i) + 0.5);\n";
    s += "                float h = sqrt(Rc * Rc + 2.0 * s * Rc * viewUp + s * s)\n";
    s += "                            - planetRadius;\n";
    s += "                h = max(h, 0.0);\n";
    s += "                opticalDepth += exp(-h / scaleHeight) * stepLen;\n";
    s += "            }\n";
    s += "        }\n";
    s += "        float fog = clamp(1.0 - exp(-opticalDepth * u_fogParams.x),\n";
    s += "                           0.0, 1.0) * heightWeight;\n";
    return s;
}

/// C++ 镜像:返回最终雾混合系数 fog ∈ [0,1]。
/// d=eye-space 视距, startDistance/density 来自 uniform, viewUp=视线×天顶,
/// camHeight=相机离地高度, planetRadius=星球半径(默认 WGS84 赤道半径)。
/// 与 aerialFogMathGLSL 同公式:4 段中点采样指数密度剖面积分光学深度。
inline double aerialFogAmountCpu(double d,
                                 double startDistance,
                                 double densityParam,
                                 double viewUp,
                                 double camHeight,
                                 double planetRadius = 6378137.0) {
    using postprocess_detail::smoothstepCpu;
    const double heightWeight = smoothstepCpu(kFogMaxHeight, 0.0, camHeight);
    const double segStart = std::max(startDistance, 0.0);
    const double segEnd = std::min(d, kFogMaxIntegrateDistance);
    double opticalDepth = 0.0;
    if (segEnd > segStart) {
        const double stepLen = (segEnd - segStart) / kFogSampleCount;
        const double Rc = planetRadius + camHeight;
        for (int i = 0; i < kFogSampleCount; ++i) {
            const double s =
                segStart + stepLen * (static_cast<double>(i) + 0.5);
            const double h = std::max(
                std::sqrt(Rc * Rc + 2.0 * s * Rc * viewUp + s * s) -
                    planetRadius,
                0.0);
            opticalDepth += std::exp(-h / kFogScaleHeight) * stepLen;
        }
    }
    double fog =
        std::clamp(1.0 - std::exp(-opticalDepth * densityParam), 0.0, 1.0);
    fog *= heightWeight;
    return fog;
}

// ============================================================
// 量 B:太阳盘红移/压暗 ramp(独立设计,勿并入 SunsetWarmthRamp 量 A)
// ============================================================
inline constexpr double kSunLowSkyKnee = 0.25;
inline constexpr double kSunsetTintRgb[3] = {1.0, 0.42, 0.16};
inline constexpr double kSunDimLow = 0.22;
inline constexpr double kDiscSunsetLow = 0.25;

/// 生成 GLSL 常量前缀(量 B 的 tint/dim/disc 系数,单一来源)。
inline std::string sunDiscSunsetConstantsGLSL() {
    using postprocess_detail::formatFloat;
    char tint[128];
    std::snprintf(tint, sizeof(tint), "const vec3 kSunsetTint = vec3(%.3f, %.3f, %.3f);\n",
                  kSunsetTintRgb[0], kSunsetTintRgb[1], kSunsetTintRgb[2]);
    return std::string(tint) +
           "const float kSunDimLow = " + formatFloat(kSunDimLow) + ";\n" +
           "const float kDiscSunsetLow = " + formatFloat(kDiscSunsetLow) +
           ";\n";
}

/// 生成完整 GLSL 函数(AtmosphereBackgroundPass 用)。
inline std::string sunLowSkyRampGLSL() {
    using postprocess_detail::formatFloat;
    return std::string("float sunLowSkyRamp(float sunElevSky) {\n") +
           "    return 1.0 - smoothstep(0.0, " +
           formatFloat(kSunLowSkyKnee) + ", max(sunElevSky, 0.0));\n" +
           "}\n";
}

/// C++ 镜像:太阳贴地平线→1,升到膝点以上→0。
inline double sunLowSkyRampCpu(double sunElevSky) {
    return 1.0 -
           postprocess_detail::smoothstepCpu(0.0, kSunLowSkyKnee,
                                              std::max(sunElevSky, 0.0));
}

}  // namespace earth_engine
