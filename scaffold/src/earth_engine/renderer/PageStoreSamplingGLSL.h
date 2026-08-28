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
// - 刀2/步3/D2 路网线场:**独立场间接纹理**(u_roadFieldIndir,同 cell 网格
//   同 indirLayer 层号;RG=场层 B=场页深度 A=ready)定位 z-封顶场页(场页
//   key 与影像页脱钩,拉近超封顶零重烘)。
//   **D2 线段纹素解算**(编码见 LineFieldRasterizer.h):每纹素一条局部
//   线段(偏移+方向角+端点余量),FS 取 2×2 邻域 4 条线段**各自解析算
//   胶囊距离取 min ——全程无插值**;MVT 折线段内即直线 → 重建精确,仅剩
//   量化误差(真实路网模拟 texelPx=4:漏画 0.28%/有害幽灵 0/误差 0.025px)。
//   全 0 纹素 = 空哨兵(失败安全);own-texel 单 fetch 早退(像素可被线
//   覆盖 ⇒ 所在纹素必有记录)。texel/px 比取 **g 的屏幕导数**÷spanF,
//   distPx = 胶囊距离(texel)/texPerPx → 线宽真屏幕像素。
//   **分级宽度**:局部 zoom = cellZoom − log2(rms),跨页连续无台阶;线半宽
//   = ramp 线性插值,**无纹素下限**(D2 精确重建后 ramp 即最终宽度;E3 元
//   规则:主路径必须真在走)。⚠️ zoom 基准=影像页 zoom,比"地图直觉 zoom"
//   高 ~2-3 档(30km 俯瞰实测 13→17),ramp 停点按此标定。
//   **历史翻车账,勿回头**(全部真机/模拟复现过):
//   ① fwidth(fieldV) 当分母:线心=场脊导数→0 → 沿线心挖洞;
//   ② dFdx(sampleUv) 当分母:span 边界回绕爆导数 → cell 网格白虚线;
//   ③ 标量距离场 + 双线性插值:跨线心尖点必高估 → 真实路网漏画 63%;
//   ④ 向量距离场:双线中轴插值过零 → 双向车道中缝幽灵;
//   ⑤ d+θ 紧凑编码(省 1 字节):θ 量化误差旋转锚点,(ox,oy) 冗余实为
//     误差解耦,勿"优化"。场关闭(params.x=0)整支死代码。
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
//   cellZoom        : cell 网格 zoom(局部 zoom 基准)。非合批管线 =
//                     roadFieldParams.y 原样转传;合批实例从 pageCellDesc
//                     解包逐实例传(批级 uniform 承首实例会在批内异 zoom
//                     瓦片上重新造出宽度台阶)
//   roadFieldParams : x=场开关 y=cellZoom(供非合批调用点转传) z=场纹理
//                     边长(texel) w=D2 偏移编码范围(texel,
//                     =kLineFieldOffsetRangeTexels);传 0 → 场支路死代码
//                     (MSL instanced 的 uniform 精简 struct 尚无场参数)
//   roadFieldWidth  : 宽度 ramp (z0, halfPx0, z1, halfPx1),halfPx=线半宽
//                     (设备px);z≤z0 取 h0、z≥z1 取 h1、之间线性
//   roadFieldColor  : 线色(RGBA 非预乘)

// GLSL 变体直接引用三个同名 sampler uniform(u_pageStore / u_pageStoreIndir /
// u_roadField;三个 GLSL 消费 shader 命名一致,注入点在声明之后可见)——省 3 个
// 实参,调用点更短。sampler 作函数参数在本机 Adreno 实测同样可用
// (eeTerrainNormalFromHeightTex 先例),纯取简洁。MSL 侧纹理是片元入口参数,
// 自由函数看不见,保持显式传递。
constexpr const char* kPageStoreSamplingGLSL = R"(
vec4 eePageStoreCompose(
    vec4 base, vec2 uvIn, vec4 geomA, vec2 geomB, vec2 phase,
    vec2 cells, int indirLayer, float cellZoom,
    vec4 roadFieldParams, vec4 roadFieldWidth, vec4 roadFieldColor) {
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
    if (roadFieldParams.x > 0.5) {
        vec4 fe = texelFetch(u_roadFieldIndir,
                             ivec3(ivec2(cell), indirLayer), 0);
        if (fe.a > 0.5) {
            int fL = int(floor(fe.r * 255.0 + 0.5) +
                         floor(fe.g * 255.0 + 0.5) * 256.0);
            vec2 spanF = vec2(exp2(floor(fe.b * 255.0 + 0.5)));
            vec2 fUv = (gGlobal - floor(gGlobal / spanF) * spanF) / spanF;
            float FT = roadFieldParams.z;
            vec2 pTex = fUv * FT;
            ivec2 own = ivec2(clamp(pTex, vec2(0.0), vec2(FT - 1.0)));
            // 哨兵早退:像素可被线覆盖 ⇒ 所在纹素必有记录(线距纹素中心
            // ≤ 覆盖半径 + √2/2 < 偏移编码范围),空纹素全 0(A=0)。
            if (texelFetch(u_roadField, ivec3(own, fL), 0).a > 0.001) {
                vec2 tPx = dFdx(g) * FT;
                vec2 tPy = dFdy(g) * FT;
                float rms = max(sqrt(0.5 * (dot(tPx, tPx) + dot(tPy, tPy))),
                                1e-6);
                float texPerPx = max(rms / spanF.x, 1e-4);
                // D2 gather-min:2×2 邻域各解一条线段的胶囊距离,取 min。
                // 无插值 → 无标量场尖点高估 / 无向量场中轴过零。
                ivec2 g0 = ivec2(floor(pTex - 0.5));
                float dTex = 1e9;
                for (int j = 0; j < 2; ++j)
                for (int i = 0; i < 2; ++i) {
                    ivec2 tc = clamp(g0 + ivec2(i, j), ivec2(0),
                                     ivec2(int(FT) - 1));
                    vec4 ft = texelFetch(u_roadField, ivec3(tc, fL), 0);
                    if (ft.a > 0.001) {
                        vec2 q = vec2(tc) + 0.5 +
                                 (ft.rg * 2.0 - 1.0) * roadFieldParams.w;
                        float th = ft.b * 3.14159265;
                        vec2 dir = vec2(cos(th), sin(th));
                        float packedA = floor(ft.a * 255.0 + 0.5);
                        float fwd = floor(packedA / 16.0) * 0.1;
                        float bck = mod(packedA, 16.0) * 0.1;
                        vec2 pq = pTex - q;
                        float dTan = dot(dir, pq);
                        float dNrm = dot(vec2(-dir.y, dir.x), pq);
                        float ex = max(max(dTan - fwd, -dTan - bck), 0.0);
                        dTex = min(dTex, length(vec2(dNrm, ex)));
                    }
                }
                float distPx = dTex / texPerPx;
                float zoomLocal = cellZoom - log2(rms);
                float wT = clamp((zoomLocal - roadFieldWidth.x) /
                                     max(roadFieldWidth.z - roadFieldWidth.x,
                                         1e-3),
                                 0.0, 1.0);
                // 无纹素下限:D2 重建精确到量化噪声,ramp 即最终宽度
                // (E3 元规则:主路径必须真在走,不再有兜底可躲)。
                float wEff = mix(roadFieldWidth.y, roadFieldWidth.w, wT);
                float roadCov = smoothstep(wEff + 0.5, wEff - 0.5, distPx) *
                                roadFieldColor.a;
                base.rgb = mix(base.rgb, roadFieldColor.rgb, roadCov);
            }
        }
    }
    return base;
}
)";

constexpr const char* kPageStoreSamplingMSL = R"(
static float4 eePageStoreCompose(
    texture2d_array<float> pageStore,
    texture2d_array<float> pageIndir,
    texture2d_array<float> roadField,
    texture2d_array<float> roadFieldIndir,
    sampler pageSampler,
    float4 base, float2 uvIn, float4 geomA, float2 geomB, float2 phase,
    float2 cells, int indirLayer, float cellZoom,
    float4 roadFieldParams, float4 roadFieldWidth, float4 roadFieldColor) {
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
    if (roadFieldParams.x > 0.5) {
        float4 fe = roadFieldIndir.read(uint2(cell), uint(indirLayer), 0);
        if (fe.a > 0.5) {
            uint fL = uint(floor(fe.r * 255.0 + 0.5) +
                           floor(fe.g * 255.0 + 0.5) * 256.0);
            float2 spanF = float2(exp2(floor(fe.b * 255.0 + 0.5)));
            float2 fUv = (gGlobal - floor(gGlobal / spanF) * spanF) / spanF;
            float FT = roadFieldParams.z;
            float2 pTex = fUv * FT;
            uint2 own = uint2(clamp(pTex, float2(0.0), float2(FT - 1.0)));
            if (roadField.read(own, fL, 0).a > 0.001) {
                float2 tPx = dfdx(g) * FT;
                float2 tPy = dfdy(g) * FT;
                float rms = max(sqrt(0.5 * (dot(tPx, tPx) + dot(tPy, tPy))),
                                1e-6);
                float texPerPx = max(rms / spanF.x, 1e-4);
                int2 g0 = int2(floor(pTex - 0.5));
                float dTex = 1e9;
                for (int j = 0; j < 2; ++j)
                for (int i = 0; i < 2; ++i) {
                    int2 tc = clamp(g0 + int2(i, j), int2(0),
                                    int2(int(FT) - 1));
                    float4 ft = roadField.read(uint2(tc), fL, 0);
                    if (ft.a > 0.001) {
                        float2 q = float2(tc) + 0.5 +
                                   (float2(ft.r, ft.g) * 2.0 - 1.0) *
                                       roadFieldParams.w;
                        float th = ft.b * 3.14159265;
                        float2 dir = float2(cos(th), sin(th));
                        float packedA = floor(ft.a * 255.0 + 0.5);
                        float fwd = floor(packedA / 16.0) * 0.1;
                        float bck = fmod(packedA, 16.0) * 0.1;
                        float2 pq = pTex - q;
                        float dTan = dot(dir, pq);
                        float dNrm = dot(float2(-dir.y, dir.x), pq);
                        float ex = max(max(dTan - fwd, -dTan - bck), 0.0);
                        dTex = min(dTex, length(float2(dNrm, ex)));
                    }
                }
                float distPx = dTex / texPerPx;
                float zoomLocal = cellZoom - log2(rms);
                float wT = clamp((zoomLocal - roadFieldWidth.x) /
                                     max(roadFieldWidth.z - roadFieldWidth.x,
                                         1e-3),
                                 0.0, 1.0);
                float wEff = mix(roadFieldWidth.y, roadFieldWidth.w, wT);
                float roadCov = smoothstep(wEff + 0.5, wEff - 0.5, distPx) *
                                float4(roadFieldColor).w;
                base.rgb = mix(base.rgb, float3(float4(roadFieldColor).xyz),
                               roadCov);
            }
        }
    }
    return base;
}
)";

} // namespace earth_engine
