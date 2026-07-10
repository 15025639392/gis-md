#pragma once

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
constexpr const char* kSkyColorGLSL = R"(
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
    // 太阳侧地平线附近微暖(前向 Mie 散射观感),仅近地平线(1-skyT)、
    // 随 spaceFactor 向太空淡出。
    float mu = max(dot(rayDir, sun), 0.0);
    sky = mix(sky, vec3(0.95, 0.90, 0.82),
              pow(mu, 8.0) * 0.30 * (1.0 - skyT) * (1.0 - spaceFactor));
    return sky;
}
)";

} // namespace earth_engine
