#pragma once

// B 方案:地形 height/normal 纹理的 GPU 烘焙片元着色器(替代 CPU
// bakeTerrainHeightNormalTexels,~6ms/瓦片 → 亚毫秒 GPU pass)。
//
// 精度已验证(scratchpad verify_bake_f32_seam.cpp):f32 烘焙的跨瓦法线缝
// = CPU double 版的 1.5°,守 2.0° 无缝阈值。⚠️关键约束:源高度纹理必须 NEAREST +
// 手动双线性(源是 uint16 拆 hi/lo 打进 RG8,硬件双线性会破坏打包),照抄
// Renderer.cpp:1069-1096 的高度纹理读取约定。
//
// 输出布局与 CPU 版逐位对齐(TerrainDisplacementTemplatePool.cpp:278-337):
//   RG = 16bit 归一化高度 t=(h-minH)/range,R=高字节 G=低字节
//   BA = 切空间法线 (-gradU·inv, -gradV·inv) 编码到 [0,1]
//
// 每个输出 texel(i,j) 直接从源纹理采 5 个点(中心+4邻居,各手动双线性),
// 不建 CPU 版的 (n+2)² scratch —— GPU 逐 fragment 并行,重复采样无所谓。
//
// ⚠️ MSL(Metal 对等)未来工作:此文件只有 GLSL。后端守卫
// (TerrainDisplacementTemplatePool::acquireHeightTexture 的
// device_->backendType()==OpenGLES)保证 Metal/Vulkan 恒回退 CPU 烘焙(正确),
// 故当前 GLES-only 无功能缺口,只是 iOS 上保留 dense descent 顿挫。补 MSL 时:
// ①加 kTerrainBakeVertMSL/kTerrainBakeFragMSL(逐字移植下方 GLSL,注意 MSL 的
//   texelFetch=tex.read(ushort2)、gl_FragCoord=[[position]]、out=[[color(0)]]);
// ②ensureBakeResources 里按 device_->backendType() 选 GLSL/MSL(照
//   Renderer.cpp 的 isMetal?MSL:GLSL 模式);③放开后端守卫允许 Metal。
// **必须在真 Metal 设备上验地形正确+无缝**再放开守卫——没设备直接放开会
// flat-terrain(本轮已踩过:createShader(GLSL) 在 Metal 失败→层不烘→变平)。

namespace earth_engine {

// 全屏 quad 顶点着色器(与 OffscreenPostProcess/TileCompositeBakePoc 同款)。
inline const char* kTerrainBakeVertGLSL() {
    return R"(#version 300 es
precision highp float;
layout(location = 0) in vec2 a_position;
void main() { gl_Position = vec4(a_position, 0.0, 1.0); }
)";
}

// 片元着色器:重采样 + 三点不等臂法线差分 → RG 高度 / BA 法线。
// uniforms:
//   u_tileTexture     : 源高度纹理(514² RGBA8,R/G=uint16 高度 hi/lo,含重叠环)
//   u_srcSize    : 源边长(如 514)
//   u_srcInset   : borderInset(如 0.5)
//   u_gridSize   : 输出网格密度(coarse 64 / dense 256);输出 n=gridSize+1
//   u_quantBase  : 高度量化原点(码 h = (quantBase+code)*quantStep)
//   u_quantStep  : 量化步长(1/8 m)
//   u_minH,u_range: 高度归一化 [0,1] 的 min/range(与 CPU 版同参)
//   u_widthM,u_heightM: 瓦片东西/南北真实地面米数(法线臂长换算)
//   u_reach      : overscanReach = inset/span(归一化,边界差分臂长)
inline const char* kTerrainBakeFragGLSL() {
    return R"(#version 300 es
precision highp float;
uniform highp sampler2D u_tileTexture;
uniform float u_srcSize;
uniform float u_srcInset;
uniform float u_gridSize;
uniform float u_quantBase;
uniform float u_quantStep;
uniform float u_minH;
uniform float u_range;
uniform float u_widthM;
uniform float u_heightM;
uniform float u_reach;
out vec4 fragColor;

// no-data 哨兵:sampleH 四角全 no-data 时的返回值(调用方用 isNoData 判)。
// 取 -1e9(远低于任何真实高程,也远离 Mapbox 的 -10000m 下限)。
const float kBakeNoData = -1.0e9;
bool isNoData(float h) { return h < -1.0e8; }

// 单点 NEAREST 取码 → 反量化成米。码 0 = no-data 哨兵 → 返回哨兵,**不是 0m**。
float decodeAt(ivec2 px) {
    px = clamp(px, ivec2(0), ivec2(int(u_srcSize) - 1));
    vec4 t = texelFetch(u_tileTexture, px, 0);
    float code = floor(t.r * 255.0 + 0.5) * 256.0 + floor(t.g * 255.0 + 0.5);
    if (code < 0.5) return kBakeNoData;  // no-data
    return (u_quantBase + code) * u_quantStep;
}

// 归一化 (u,v) → 源像素 → 手动双线性(unclamped:±reach 内读重叠环真实邻瓦值)。
// 反量化后再插值(与 CPU sampleBilinearUnclamped 对齐:对米值双线性,非对码)。
//
// ⚠️ no-data 角**剔除后重归一化**(与 CPU sampleBilinearUnclamped 同款,那边的
// 注释解释了为什么:哨兵直接参与 mix 会得到一个"中间值",既不是真高度也不再被
// isNoData 认出来)。此前 GPU 版把 no-data 当 0m 混进去 —— 生产 514 源上整列
// nodata 的重叠环于是把瓦片边界高度砍掉一半(实测 908m→454m),与 CPU 路径行为
// 分叉。四角全 no-data → 返回哨兵,由调用方决定语义。
float sampleH(float u, float v) {
    float span = u_srcSize - 1.0 - 2.0 * u_srcInset;
    float fx = u * span + u_srcInset;
    float fy = v * span + u_srcInset;
    float x0 = floor(fx), y0 = floor(fy);
    float tx = fx - x0, ty = fy - y0;
    float h[4];
    h[0] = decodeAt(ivec2(int(x0),     int(y0)));
    h[1] = decodeAt(ivec2(int(x0) + 1, int(y0)));
    h[2] = decodeAt(ivec2(int(x0),     int(y0) + 1));
    h[3] = decodeAt(ivec2(int(x0) + 1, int(y0) + 1));
    float w[4];
    w[0] = (1.0 - tx) * (1.0 - ty);
    w[1] = tx * (1.0 - ty);
    w[2] = (1.0 - tx) * ty;
    w[3] = tx * ty;
    float sum = 0.0, wsum = 0.0;
    for (int k = 0; k < 4; ++k) {
        if (!isNoData(h[k])) { sum += h[k] * w[k]; wsum += w[k]; }
    }
    return wsum > 0.0 ? sum / wsum : kBakeNoData;
}

// 三点不等臂一阶导(逐字移植 CPU derivative3pt,臂相等时退化为标准中心差分)。
float deriv3(float fL, float fC, float fR, float dL, float dR) {
    if (dL <= 0.0) return dR > 0.0 ? (fR - fC) / dR : 0.0;
    if (dR <= 0.0) return (fC - fL) / dL;
    return (-dR * dR * fL + (dR * dR - dL * dL) * fC + dL * dL * fR) /
           (dL * dR * (dL + dR));
}

void main() {
    // 本 fragment = 输出 texel (i,j),i,j ∈ [0, n-1],n = gridSize+1。
    int i = int(gl_FragCoord.x);
    int j = int(gl_FragCoord.y);
    float g = u_gridSize;

    // scratch 轴:节点 k 的归一化坐标 = k/gridSize;边界(i=0 / i=n)左右臂踩重叠环。
    float uC = float(i) / g;
    float vC = float(j) / g;
    float uL = (i == 0)            ? -u_reach       : float(i - 1) / g;
    float uR = (i == int(g))       ? 1.0 + u_reach  : float(i + 1) / g;
    float vT = (j == 0)            ? -u_reach       : float(j - 1) / g;  // 北(v 小)
    float vB = (j == int(g))       ? 1.0 + u_reach  : float(j + 1) / g;

    float hC = sampleH(uC, vC);
    float hL = sampleH(uL, vC), hR = sampleH(uR, vC);
    float hT = sampleH(uC, vT), hB = sampleH(uC, vB);
    bool cOk = !isNoData(hC);
    if (!cOk) { hC = 0.0; }  // 无数据处落海平面(Mapbox 缺数据语义)

    // 臂长(米):节点间距 × 真实地面跨度。
    // ⚠️ no-data 邻居 → 臂长置 0 = 丢掉这条臂,deriv3 退化为另一侧单边差分。
    // 拿海平面 0m 去和真实地表求斜率会在 nodata 边界造假悬崖:边界臂只有
    // reach×跨度(514 源 z7 ≈266m),900m 假落差直接把法线打到近水平。生产源
    // 抽样 19 片有 14 片的西/北重叠环整列 code=0,故这是常态不是边角。
    // (与 CPU bakeTerrainHeightNormalTexels 的 nodeValid 分支逐条对应。)
    float dLu = (cOk && !isNoData(hL)) ? (uC - uL) * u_widthM  : 0.0;
    float dRu = (cOk && !isNoData(hR)) ? (uR - uC) * u_widthM  : 0.0;
    float dLv = (cOk && !isNoData(hT)) ? (vC - vT) * u_heightM : 0.0;
    float dRv = (cOk && !isNoData(hB)) ? (vB - vC) * u_heightM : 0.0;
    float gradU = deriv3(hL, hC, hR, dLu, dRu);
    float gradV = deriv3(hT, hC, hB, dLv, dRv);
    float inv = inversesqrt(gradU * gradU + gradV * gradV + 1.0);

    // RG = 16bit 高度打包(与 CPU lround(t*65535) 的 hi/lo 一致)。
    float t = clamp((hC - u_minH) / u_range, 0.0, 1.0);
    float v16 = floor(t * 65535.0 + 0.5);
    float hi = floor(v16 / 256.0);
    float lo = v16 - hi * 256.0;

    // BA = 切空间法线,c*0.5+0.5 编码(与 CPU enc 一致)。
    float nx = clamp((-gradU * inv) * 0.5 + 0.5, 0.0, 1.0);
    float ny = clamp((-gradV * inv) * 0.5 + 0.5, 0.0, 1.0);

    fragColor = vec4(hi / 255.0, lo / 255.0, nx, ny);
}
)";
}

}  // namespace earth_engine
