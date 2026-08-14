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
//   (0=miss 回落 mappedRaster / 128=影像驻留场 pending / 255=双就绪)。
// - per-cell 渐变 LOD(§16.3):d>0 采粗祖先页;祖先子区原点必须在**全局**
//   源瓦片下标上算(gGlobal = g + phase),否则 d>0 采错子区(块状棋盘格)。
// - 影像 factor = step(0.3, A):128/255 都显影像(面走快路)。
// - 刀2 路网 SDF 场:同一次间接查找/同层号再采 R8 场(编码=归一化中心线
//   距离,1=线心 0=远,失败安全);gate A>0.6(须双就绪)。**像素解算**
//   (场线宽像素一致专项 2026-08-14):texel/px 比取自 **g(几何→源格
//   仿射)的屏幕导数**再 ÷span,distPx = (1−fieldV)·band/texPerPx,按
//   线半宽(px)阈值 + 0.5px AA —— 页内近端放大/祖先页兜底/页界跳档全被
//   导数自动补偿,线宽真屏幕像素恒定。分母的两次真机翻车,勿回头:
//   ① fwidth(fieldV):场值在中心线是脊,脊上导数→0,eps 兜底把线心解算
//     成"远"→ 沿线心周期性挖洞;
//   ② dFdx(sampleUv):sampleUv 在每个 span 边界 1→0 回绕,跨界 quad 导数
//     爆炸 → distPx→0 → 无条件画白 → 沿 cell 网格的白虚线("瓦片网格线")。
//   g 是 uv 的仿射,跨 cell 连续无回绕,÷span 完成祖先页纹素换算。
//   band 外全 0 区:分子饱和为 band px 级 > 任何线宽 → cov=0,退化方向是
//   "无线"而非脏色。
//   **深度放大下限(真机翻车③)**:纹素 >> 线宽时,线心脊的纹素采样相位
//   让 distPx 沿线在阈值两侧振荡(纹素中心恰在线上→亮,偏 0.5 纹素→灭)
//   → 线碎成点串。半宽/AA 各设纹素下限(0.6/0.35 texel):极端放大时线
//   随纹素适度变粗+羽化,连续保形;正常 1:1 态两下限恰不生效(0.6<1.75、
//   0.35<0.5),零影响。场关闭(params.x=0)整支死代码。
// - 混合式 = alphaOver 语义内联(不依赖各 shader 的同名帮助函数):
//   overlay.a·=clamp(factor);rgb=mix;a=max —— 与六处旧内联逐字节一致。
//
// 参数全显式传入、不读 uniform/varying → 各管线的数据来源(per-draw uniform
// vs per-instance 顶点属性流)与本函数解耦;某管线漏配某输入会在编译期
// (缺实参)或对照测试(test_pipeline_feature_contracts)现形,而非上屏后
// 时隐时现。
//   base            : 已合成 mappedRaster 的底色
//   uvIn            : 几何/texcoord UV(已完成 clip-remap,未 clamp)
//   geomA/geomB     : 仿射系数 (c0.xy, dU.xy) / (dV.xy)
//   phase           : 祖先寻址相位(x0/y0 mod 2^kMaxDetDepthLevels)
//   cells           : cell 网格尺寸(已 max(1))
//   indirLayer      : 该瓦片的间接纹理 array 层
//   roadFieldParams : x=场开关 y=线半宽(设备px) z=场纹理边长(texel)
//                     w=编码带宽(texel,=kLineFieldBandTexels);传 0 →
//                     场支路死代码(MSL instanced 的 uniform 精简 struct
//                     尚无场参数,即取此路)
//   roadFieldColor  : 线色(RGBA 非预乘)

// GLSL 变体直接引用三个同名 sampler uniform(u_pageStore / u_pageStoreIndir /
// u_roadField;三个 GLSL 消费 shader 命名一致,注入点在声明之后可见)——省 3 个
// 实参,调用点更短。sampler 作函数参数在本机 Adreno 实测同样可用
// (eeTerrainNormalFromHeightTex 先例),纯取简洁。MSL 侧纹理是片元入口参数,
// 自由函数看不见,保持显式传递。
constexpr const char* kPageStoreSamplingGLSL = R"(
vec4 eePageStoreCompose(
    vec4 base, vec2 uvIn, vec4 geomA, vec2 geomB, vec2 phase,
    vec2 cells, int indirLayer,
    vec4 roadFieldParams, vec4 roadFieldColor) {
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
    if (roadFieldParams.x > 0.5 && e.a > 0.6) {
        float fieldV = texture(u_roadField, vec3(sampleUv, layer)).r;
        vec2 tPx = dFdx(g) * roadFieldParams.z / span.x;
        vec2 tPy = dFdy(g) * roadFieldParams.z / span.x;
        float texPerPx = max(
            sqrt(0.5 * (dot(tPx, tPx) + dot(tPy, tPy))), 1e-4);
        float distPx = (1.0 - fieldV) * roadFieldParams.w / texPerPx;
        float texelPx = 1.0 / texPerPx;
        float wEff = max(roadFieldParams.y, 0.6 * texelPx);
        float aa = max(0.5, 0.35 * texelPx);
        float roadCov = smoothstep(wEff + aa, wEff - aa, distPx) *
                        e.a * roadFieldColor.a;
        base.rgb = mix(base.rgb, roadFieldColor.rgb, roadCov);
    }
    return base;
}
)";

constexpr const char* kPageStoreSamplingMSL = R"(
static float4 eePageStoreCompose(
    texture2d_array<float> pageStore,
    texture2d_array<float> pageIndir,
    texture2d_array<float> roadField,
    sampler pageSampler,
    float4 base, float2 uvIn, float4 geomA, float2 geomB, float2 phase,
    float2 cells, int indirLayer,
    float4 roadFieldParams, float4 roadFieldColor) {
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
    if (roadFieldParams.x > 0.5 && e.a > 0.6) {
        float fieldV = roadField.sample(pageSampler, sampleUv, uint(layer)).r;
        float2 tPx = dfdx(g) * roadFieldParams.z / span.x;
        float2 tPy = dfdy(g) * roadFieldParams.z / span.x;
        float texPerPx = max(
            sqrt(0.5 * (dot(tPx, tPx) + dot(tPy, tPy))), 1e-4);
        float distPx = (1.0 - fieldV) * roadFieldParams.w / texPerPx;
        float texelPx = 1.0 / texPerPx;
        float wEff = max(roadFieldParams.y, 0.6 * texelPx);
        float aa = max(0.5, 0.35 * texelPx);
        float roadCov = smoothstep(wEff + aa, wEff - aa, distPx) *
                        e.a * float4(roadFieldColor).w;
        base.rgb = mix(base.rgb, float3(float4(roadFieldColor).xyz), roadCov);
    }
    return base;
}
)";

} // namespace earth_engine
