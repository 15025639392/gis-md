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
//
// ── 两个变体:LDR(默认)vs HDR(kEnableHdrPipeline)──────────────────
// withTerrainLight() 按 PipelineConfig.h 的 kEnableHdrPipeline 选注入哪个:
//   flag OFF → kTerrainLight*   :gamma 空间直算,输出显示色(**现状 = T0,
//              零变化**;shadowFloor=0.3,不 decode/encode)。
//   flag ON  → kTerrainLightHdr*:base decode 到线性 → 线性域光照 → **输出线性
//              HDR**(sRGB encode 移到全屏 tonemap 终端)。srgbToLinear 仅作用
//              于 color 纹理;heightmap/normal 等数据纹理不经此函数,天然不 decode
//              (角色感知,见设计文档 §3)。⚠️ HDR 变体常数(shadowFloor=0.15/
//              ambientScale=0.6)是 **provisional**,真正调参在 T2 对着 tonemap
//              输出做一次(见设计文档 §9)。pow(2.2) 近似同 czm_srgbToLinear。

// —— LDR(默认,= T0 gamma,零变化)——
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

// —— HDR(kEnableHdrPipeline):base→线性,输出线性 HDR,常数 provisional ——
constexpr const char* kTerrainLightHdrGLSL = R"(
vec3 srgbToLinear(vec3 c) { return pow(max(c, vec3(0.0)), vec3(2.2)); }
vec3 terrainSurfaceLight(vec3 baseRgb, float NdotL, vec3 ambient) {
    vec3 albedo = srgbToLinear(baseRgb);
    const float kLambertGain = 0.9;
    const float kShadowFloor = 0.15;  // provisional(T0=0.3);T2 tonemap 后重调
    const float kAmbientScale = 0.6;  // provisional;线性下收暗部补光
    float directional = clamp(NdotL * kLambertGain + kShadowFloor, 0.0, 1.0);
    float sunlit = clamp(NdotL, 0.0, 1.0);
    vec3 sunTint = vec3(1.05, 1.0, 0.91);
    return albedo * directional * mix(vec3(1.0), sunTint, sunlit)
         + albedo * (ambient * kAmbientScale) * (1.0 - sunlit);  // 线性 HDR
}
)";

constexpr const char* kTerrainLightHdrMSL = R"(
float3 srgbToLinear(float3 c) { return pow(max(c, float3(0.0)), float3(2.2)); }
float3 terrainSurfaceLight(float3 baseRgb, float NdotL, float3 ambient) {
    float3 albedo = srgbToLinear(baseRgb);
    const float kLambertGain = 0.9;
    const float kShadowFloor = 0.15;  // provisional(T0=0.3);T2 tonemap 后重调
    const float kAmbientScale = 0.6;  // provisional;线性下收暗部补光
    float directional = clamp(NdotL * kLambertGain + kShadowFloor, 0.0, 1.0);
    float sunlit = clamp(NdotL, 0.0, 1.0);
    float3 sunTint = float3(1.05, 1.0, 0.91);
    return albedo * directional * mix(float3(1.0), sunTint, sunlit)
         + albedo * (ambient * kAmbientScale) * (1.0 - sunlit);  // 线性 HDR
}
)";

} // namespace earth_engine
