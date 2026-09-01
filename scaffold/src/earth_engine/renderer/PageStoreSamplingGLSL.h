#pragma once

namespace earth_engine {

// ============================================================
// 共享页存储采样 + 路网场解算(GLSL/MSL 源,注入进六个地形片元 shader)
// ============================================================
//
// **页存储采样的唯一治理点**。gltf / terrain(位移)/ terrainInstanced ×
// GLES/Metal 六个片元 shader 曾各自内联一份相同的采样链(间接纹理定位 →
// 祖先 scale-bias → 页采样 → 场解算),历史上已两次在其中单条管线上漏配
// (合批漏拷场 uniform 9043e20fe;位移路径几何 UV 喂 details 标定 80892aca1)。
// 此处收成单一函数,`withPageStoreSampling()` 在编译前注入到各 shader 片元
// 入口之前(照 withTerrainLight / TerrainSurfaceLightGLSL.h 的拼接惯例)。
// 改采样链 / 加特性只改这一处,六个 shader 自动同步。
//
// 采样链语义(原六份注释合并,单点维护):
// - cell 网格 = **影像源瓦片网格**(单位:源瓦片),不是几何瓦片等分。
// - g = geomA.xy + u·geomA.zw + v·geomB:几何 UV → 源格的逐瓦仿射
//   ([瓦界对齐] 含交叉项;详见 TerrainPageStore::TileIndir::geomAffine)。
//   details 逐顶点 texcoord 变体(gltf 真实网格)以轴对齐退化形式传入
//   geomA=(origin.xy, span.x, 0)、geomB=(0, span.y) → g = origin + uv·span,
//   与旧式逐位等价(+0 项被 IEEE 吸收)。
// - 间接纹理单次整数 fetch 定位 array 层;RGBA8 解码 R+G·256;A 通道三态
//   (0=miss 回落 directComposite / 128=影像驻留场 pending / 255=双就绪)。
// - per-cell 渐变 LOD(§16.3):d>0 采粗祖先页;祖先子区原点必须在**全局**
//   源瓦片下标上算(gGlobal = g + phase),否则 d>0 采错子区(块状棋盘格)。
// - 影像 factor = step(0.3, A):128/255 都显影像(面走快路)。
// - 混合式 = alphaOver 语义内联(不依赖各 shader 的同名帮助函数):
//   overlay.a·=clamp(factor);rgb=mix;a=max —— 与六处旧内联逐字节一致。
//
// 参数全显式传入、不读 uniform/varying → 各管线的数据来源(per-draw uniform
// vs per-instance 顶点属性流)与本函数解耦;某管线漏配某输入会在编译期
// (缺实参)或对照测试(test_pipeline_feature_contracts)现形,而非上屏后
// 时隐时现。
//   base            : 已合成 directComposite 的底色
//   uvIn            : 几何/texcoord UV(已完成 clip-remap,未 clamp)
//   geomA/geomB     : 仿射系数 (c0.xy, dU.xy) / (dV.xy)
//   phase           : 祖先寻址相位(x0/y0 mod 2^kMaxDetDepthLevels)
//   cells           : cell 网格尺寸(已 max(1))
//   indirLayer      : 该瓦片的间接纹理 array 层

// GLSL 变体直接引用同名 sampler uniform(u_pageStore / u_pageStoreIndir)。
// sampler 作函数参数在本机 Adreno 实测同样可用
// (eeTerrainNormalFromHeightTex 先例),纯取简洁。MSL 侧纹理是片元入口参数,
// 自由函数看不见,保持显式传递。
constexpr const char* kPageStoreSamplingGLSL = R"(
vec4 eePageStoreCompose(
    vec4 base, vec2 uvIn, vec4 geomA, vec2 geomB, vec2 phase,
    vec2 cells, int indirLayer) {
    vec2 uvc = clamp(uvIn, 0.0, 1.0);
    vec2 g = geomA.xy + uvc.x * geomA.zw + uvc.y * geomB;
    vec2 cell = clamp(floor(g), vec2(0.0), cells - vec2(1.0));
    vec4 e = texelFetch(u_pageStoreIndir, ivec3(ivec2(cell), indirLayer), 0);
    float layer = floor(e.r * 255.0 + 0.5) + floor(e.g * 255.0 + 0.5) * 256.0;
    float d = floor(e.b * 255.0 + 0.5);
    vec2 span = vec2(exp2(d));
    vec2 gGlobal = g + phase;
    vec2 origin = floor(gGlobal / span) * span;
    vec2 sampleUv = (gGlobal - origin) / span;
    vec4 page = texture(u_pageStore, vec3(sampleUv, layer));
    page.a *= clamp(step(0.3, e.a), 0.0, 1.0);
    base.rgb = mix(base.rgb, page.rgb, page.a);
    base.a = max(base.a, page.a);
    return base;
}
)";

constexpr const char* kPageStoreSamplingMSL = R"(
static float4 eePageStoreCompose(
    texture2d_array<float> pageStore,
    texture2d_array<float> pageIndir,
    sampler pageSampler,
    float4 base, float2 uvIn, float4 geomA, float2 geomB, float2 phase,
    float2 cells, int indirLayer) {
    float2 uvc = clamp(uvIn, 0.0, 1.0);
    float2 g = float2(geomA.x, geomA.y) + uvc.x * float2(geomA.z, geomA.w) +
               uvc.y * geomB;
    float2 cell = clamp(floor(g), float2(0.0), cells - float2(1.0));
    float4 e = pageIndir.read(uint2(cell), uint(indirLayer), 0);
    float layer = floor(e.r * 255.0 + 0.5) + floor(e.g * 255.0 + 0.5) * 256.0;
    float d = floor(e.b * 255.0 + 0.5);
    float2 span = float2(exp2(d));
    float2 gGlobal = g + phase;
    float2 origin = floor(gGlobal / span) * span;
    float2 sampleUv = (gGlobal - origin) / span;
    float4 page = pageStore.sample(pageSampler, sampleUv, uint(layer));
    page.a *= clamp(step(0.3, e.a), 0.0, 1.0);
    base.rgb = mix(base.rgb, page.rgb, page.a);
    base.a = max(base.a, page.a);
    return base;
}
)";

} // namespace earth_engine
