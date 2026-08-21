#pragma once

#include "SunsetWarmthRamp.h"

namespace earth_engine {

// ============================================================
// 共享天空色模型(GLSL 源,字符串拼接进两个全屏 shader)
// ============================================================
//
// **统一散射的唯一治理点**。大气背景 pass 与 aerial fog **同调**一个
// computeSkyColor:雾霾色 = 该视线方向的天空色,逐分量恒等 → 天空↔地形
// 交接无缝、无色差、无需常数对齐(Cesium `fogColor = groundAtmosphereColor`
// 思路)。改天空色板只改这一处,两个 shader 自动同步。
//
// 只含**方向性天空底色**(仰角渐变 + 太空压黑 + 太阳侧地平线微暖),不含
// 太阳盘 / 光晕 / crepuscular rays / limb rim —— 那些是天空 pass 专属、
// 局部(太阳中心或地球边缘)、fog 不需要,仍留在 AtmosphereBackgroundPass。
//
// 参数全显式传入、不读 uniform → 两 shader 各自的 uniform 命名(u_camPos vs
// u_planetRadius 等)与本函数解耦。
//   rayDir      : 视线方向(世界 / ECEF, 单位向量)
//   localUp     : normalize(camPos) 当地天顶(单位向量)
//   sun         : 太阳方向(ECEF, 单位向量)
//   spaceFactor : 0 = 近地(浓大气) → 1 = 太空(深黑)。两 caller 都用
//                 smoothstep(120000.0, 900000.0, camHeight) 计算,保持一致。
//
// ⚠️ 修改约定:horizonSky / zenithSky / 太阳暖色是此模型的**唯一**色板来源。
// 不要在 caller 里再引入独立的 lowSkyColor / horizonColor 常数(那正是旧的
// "三套模型凑近似"病根)。
/// 生成共享天空色模型 GLSL 源(computeSkyColor + 暖度曲线)。
/// 由 2026-08-21(L-P2)从 constexpr 文本改为生成函数:日落暖度曲线
/// sunsetWarmthRamp 的定义由 SunsetWarmthRamp.h 的单一常量生成,杜绝
/// "CPU/GLSL 各抄一份 smoothstep 0→0.30"的历史漂移债。
inline std::string kSkyColorGLSL() {
    return sunsetWarmthRampGLSL() + R"(
vec3 computeSkyColor(vec3 rayDir, vec3 localUp, vec3 sun, float spaceFactor) {
    float viewUp = dot(rayDir, localUp);
    // 地平线雾白蓝 → 天顶深蓝的仰角(viewUp)渐变。ray-based —— 与 fog 从
    // 深度重建视线的参数化同一套,故地平线(viewUp=0)处两者恒等收敛。
    vec3 horizonSky = vec3(0.68, 0.79, 0.86);
    vec3 zenithSky  = vec3(0.06, 0.24, 0.55);
    float skyT = smoothstep(0.0, 0.85, viewUp);
    vec3 sky = mix(horizonSky, zenithSky, pow(skyT, 0.85));
    // 太空:大气变薄,整体压向近黑。
    sky = mix(sky, vec3(0.0, 0.005, 0.025), spaceFactor);

    // ---- 日落着色:随太阳高度从"日间微暖"过渡到"日落橙" ----
    // sunElev = 太阳相对当地天顶的高度(用相机天顶近似全天);sunLow 由
    // sunsetWarmthRamp(与 CPU 同一曲线,见 SunsetWarmthRamp.h)在太阳贴
    // 地平线时→1、升到膝点以上→0。高日光下 sunLow=0,以下各项全部归零
    // → 与旧行为逐字等价(仅低太阳场景着色)。
    float mu = max(dot(rayDir, sun), 0.0);
    float sunElev = dot(sun, localUp);
    float sunLow = sunsetWarmthRamp(sunElev);
    // 暖色向天顶收紧:lowSky 贴地平线≈1、升高按 2.5 次幂快速→0 → 暖色集中在
    // 地平线带,高空回到 base(蓝),给"近地平线暖、高空冷"的日落层次。
    float lowSky = pow(1.0 - skyT, 2.5);
    // 暖色**单一来源**(不引入平行 lowSkyColor 常数):日间→pale、日落→深橙。
    vec3 warm = mix(vec3(0.95, 0.90, 0.82), vec3(1.00, 0.48, 0.20), sunLow);
    // 朝阳前向散射辉光:高日光紧致(pow8);日落时叠加展宽项(pow3)成朝阳侧橙霞
    // (pow3 比 pow2 更集中朝阳、反阳侧衰减更快)。
    float glow = pow(mu, 8.0) * 0.30 + pow(mu, 3.0) * 0.60 * sunLow;
    sky = mix(sky, warm, clamp(glow * lowSky * (1.0 - spaceFactor), 0.0, 0.92));
    // 日落时整条地平线(各方位)再敷一层弱暖洗 → 日落氛围而非仅朝阳侧。lowSky
    // 已含幂收紧,幅度小(0.20)避免整片天变橙。
    sky = mix(sky, vec3(0.80, 0.52, 0.45),
              sunLow * lowSky * 0.20 * (1.0 - spaceFactor));
    // 日落时天顶侧压向冷蓝紫,强化冷暖对比(仅高空 skyT、随 sunLow)。
    sky = mix(sky, vec3(0.10, 0.16, 0.42), sunLow * skyT * 0.25 * (1.0 - spaceFactor));
    return sky;
}
)";
}

} // namespace earth_engine
