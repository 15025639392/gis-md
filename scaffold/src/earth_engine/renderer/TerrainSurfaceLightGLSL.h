#pragma once

namespace earth_engine {

// ============================================================
// 共享地形表面光照模型(GLSL/MSL 源,注入进四个地形片元 shader)
// ============================================================
//
// **地形光照的唯一治理点**。terrainShader / terrainInstancedShader × GLES/Metal
// 四个片元 shader 曾各自内联一份完全相同的 GE 式半球光照(0.9/0.3/sunTint 常
// 数复制 4 份);此处收成单一函数,`withTerrainLight()` 在编译前把它注入到各
// shader 的片元入口之前(照 computeSkyColor / AtmosphereSkyColorGLSL.h 的拼接
// 惯例)。改地形日照色板 / 增益只改这一处,四个 shader 自动同步。
//
// GE 式半球光照:蓝天 ambient 补光集中在阴影侧,太阳做方向 relief。受光面≈base
// (不过曝、不蓝 cast);阴影侧被抬 + 天空蓝染,而非洗白或死黑。directional:
// 0=背光→1=受光。方向项 = 线性 lambert × 增益 + 阴影底,clamp 收尾。系数取自
// CesiumJS Globe.js 默认(lambertDiffuseMultiplier=0.9 / vertexShadowDarkness
// =0.3),对应 GlobeFS.glsl 的 ENABLE_VERTEX_LIGHTING 路径。范围 0.3→1.0(3.3×)。
//
// **不用 smoothstep**:它在 NdotL→1(高太阳角,最常见)处导数恰为 0,会把地形
// 法线的全部贡献吃掉。实测本 demo 钉死场景 directional 中位 0.992,导致法线改
// 动前后画面只有 0.16% 像素有差(见 docs/issues/
// terrain-visual-maturity-gap-2026-08-02.md §4.4)。osgEarth(PhongLighting.glsl)
// 同样用线性 max(dot(N,L),0) —— 两个参考实现在"线性"上一致。
//
// 暖阳/冷阴影(GE/摄影级):受光面乘微暖太阳色(红+蓝−),背光面由天空蓝
// ambient 补光——冷暖分离让 relief 更立体、更像真实日照。冷暖/ambient 的分配
// 按纯 NdotL(sunlit),与亮度增益(directional)解耦。
//
// 参数全显式传入、不读 uniform → 各 shader 的 uniform 命名(u_ambient vs
// u.ambient)与本函数解耦。
//   baseRgb : 反照率(影像/底色,含 mappedRaster 合成后)
//   NdotL   : 表面法线·太阳方向(未 clamp,函数内自取受光/方向两分量)
//   ambient : 天空蓝补光色(u_ambient.rgb / u.ambient.rgb)
constexpr const char* kTerrainLightGLSL = R"(
vec3 terrainSurfaceLight(vec3 baseRgb, float NdotL, vec3 ambient) {
    const float kLambertGain = 0.9;   // = Cesium lambertDiffuseMultiplier
    const float kShadowFloor = 0.3;   // = Cesium vertexShadowDarkness
    float directional = clamp(NdotL * kLambertGain + kShadowFloor, 0.0, 1.0);
    float sunlit = clamp(NdotL, 0.0, 1.0);
    vec3 sunTint = vec3(1.05, 1.0, 0.91);
    return baseRgb * directional * mix(vec3(1.0), sunTint, sunlit)
         + baseRgb * ambient * (1.0 - sunlit);
}
)";

constexpr const char* kTerrainLightMSL = R"(
float3 terrainSurfaceLight(float3 baseRgb, float NdotL, float3 ambient) {
    const float kLambertGain = 0.9;   // = Cesium lambertDiffuseMultiplier
    const float kShadowFloor = 0.3;   // = Cesium vertexShadowDarkness
    float directional = clamp(NdotL * kLambertGain + kShadowFloor, 0.0, 1.0);
    float sunlit = clamp(NdotL, 0.0, 1.0);
    float3 sunTint = float3(1.05, 1.0, 0.91);
    return baseRgb * directional * mix(float3(1.0), sunTint, sunlit)
         + baseRgb * ambient * (1.0 - sunlit);
}
)";

} // namespace earth_engine
