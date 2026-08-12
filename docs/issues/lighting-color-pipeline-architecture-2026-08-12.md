# 光照 / 颜色管线统一架构设计 — 2026-08-12

## 0. 背景与目标

起因:全工程光照/颜色处理呈"经验公式各自为政 + 无颜色管线"状态,五条现状经本地核实(现读,非记忆):

| 现状 | 证据(file:line) |
|---|---|
| 无 HDR framebuffer,全链 8-bit unorm | Format enum 仅 `{RGBA8,RGB8,R8,Depth32F}` `scaffold/src/earth_engine/renderer/RenderDevice.h:236`;离屏 FBO=`GL_RGBA8` `scaffold/src/earth_engine/platform/android/RenderDeviceGLES.cpp:607`;Metal=`RGBA8/BGRA8Unorm` |
| 无统一 exposure / tone mapping | 全树无 ACES/Reinhard/Filmic;唯一 `expose()` 是 SkyGradient **CPU 烘天空色**助手 `scaffold/src/earth_engine/environment/SkyGradient.cpp:56`,不作用于场景帧 |
| 离屏后处理只实现 3 个 effect | enum `{Passthrough,Fxaa,AerialFog}` `scaffold/src/earth_engine/renderer/OffscreenPostProcess.h:28`,三者全真实现;AA/HDR/bloom/大气仅注释规划名目,**连 enum 成员/stub 都没有** |
| Metal 后端离屏后处理禁用 | `supportsOffscreenPostProcess() { return false; }` `scaffold/src/earth_engine/platform/ios/RenderDeviceMetal.h:22`;根因=全屏 shader 只有 GLSL 无 MSL;Engine 全局门控 `scaffold/src/earth_engine/Engine.cpp:113` |
| 各 shader 经验光照公式互不统一,且 terrain 自身跨后端复制 4 份 | terrain `NdotL*0.9+0.3`+sunTint 半球光,4 份:`Renderer.cpp:1257/1575/3624/3872`;glTF `0.38*occlusion+0.62*smoothstep(NdotL)`+ambient,2 份:`Renderer.cpp:729/3236`;夜/太空 `exp(elev*8)`+平滑步 `SceneRenderPipeline.cpp:331`;**唯一跨 shader 复用是天空色** `computeSkyColor`(`environment/AtmosphereSkyColorGLSL.h`),无共享表面光照单元 |

**核心重定义**:该统一的是**颜色管线**(线性工作流 + 可选 HDR 中间靶 + 一道 tone mapping),**不是把 terrain 与 glTF 压成一条 BRDF**。它们的着色差异(硬终结线 vs 软+AO)是真实现象差异,需要的"一致"是同一颜色空间/曝光/tonemap 曲线,而非同一公式。

**目标**:定架构方向 + 分期 + 重调成本清单,供裁决"做到哪一层"。本文不含代码落地,仅设计。

## 1. 成熟引擎基准(.ref 源码调研,四个 agent 各带 file:line 证据)

| 引擎 | HDR float 靶 | tonemap 曲线 | 曝光 | 线性工作流 | 统一 BRDF? |
|---|---|---|---|---|---|
| **cesium-js** | 可选,**默认关**;half-float,降级 full-float→8bit(`Scene/SceneFramebuffer.js:61-65`);GPU 支持才开(`Scene/Scene.js:1611-1619`),默认 `false`(`Scene.js:742`) | **PBR-Neutral**(默认,`Scene/PostProcessStageCollection.js:49,310`),1.121 从 ACES 迁移(`CHANGES.md:676-677`);**非 HDR 路径也逐 shader 调**(`Shaders/GlobeFS.glsl:528-531`) | 固定 1.0;auto-exposure 有代码但**强制禁用**(`PostProcessStageCollection.js:40-44`,注释"多数 shader 输出已在 [0,1]") | 是,**按纹理角色**选择性解码:base/emissive `czm_srgbToLinear`(`Shaders/Model/MaterialStageFS.glsl:202,221`),normal/MR/occlusion **裸采样不解码**(`MaterialStageFS.glsl:71,298,500`) | ❌ globe 经验 Lambert(`GlobeFS.glsl:438,441`,`*0.9+0.3` 与本项目 terrain 同式)/ glTF 完整 Cook-Torrance(`czm_pbrLighting`,`Model/LightingStageFS.glsl:66`),**零交叉调用** |
| **Skybolt** | **常开** `GL_RGBA16F`(`RenderOperation/DefaultRenderCameraViewport.cpp:52`) | Uchimura(GT filmic,`Assets/Core/Shaders/ToneMapping.h:27-48`),单次 composite(`CompositeFinal.frag:34`) | 固定 ×4.0(`ToneMapping.h:80,90`),无 auto-exposure | 是,线性 HDR 累进 → 末端 sRGB 编码(`ToneMapping.h:15-23`) | ❌(只有 PBR 表面,无 terrain 对照)|
| **maplibre-gl-js** | ❌ 无(唯一 RGBA16F 是 heatmap 模糊累加器 `src/webgl/draw/draw_heatmap.ts:220`);其余全 `RGBA8`(`src/webgl/texture.ts:81`) | ❌ 无(仅天空 shader 自封闭 exposure `src/shaders/glsl/atmosphere.fragment.glsl:167-170`) | — | ❌ gamma 空间 | ❌ 经典 |
| **osgearth** | ❌ 无 | ❌ 无(唯一 exposure 是死注释 `src/osgEarth/VisibleLayer.cpp:86-91`) | — | ❌ gamma 空间(`PhongLighting.glsl` 无 gamma/srgb token) | ❌ Phong |

## 2. 两条铁结论

1. **统一 BRDF —— 四个参考全部否决**。无一引擎把 terrain 与 glTF 合成一条公式;Cesium globe 用的正是本项目现有的 `Lambert*0.9+0.3`。"统一光照 = 一条 BRDF"方向作废。**该统一的是管线,terrain/glTF 表面模型永久分离。**

2. **HDR float 靶 —— 是可选上层,不是地基**。Cesium 默认关、maplibre/osgEarth 根本不做,仅 Skybolt(电影级)常开。生产级地图/地球引擎多数视其为"不必要开销"。

3. **关键解耦(纠正早期分层)**:Cesium 在 8-bit 路径下**逐 shader** 调 PBR-Neutral tonemap → **tonemap 曲线与线性工作流不依赖 HDR float 靶**。此前"没有 HDR 地基硬做统一拿不到收益"的判断被证伪一半:线性 + tonemap 可先在现管线兑现,HDR float 靶是可分离的更贵上层。

## 3. 架构分层(按成本/价值,已解耦)

| 层 | 内容 | 需 float 靶/Metal 离屏? | 价值 | 成本 |
|---|---|---|---|---|
| **T0** | 代码统一:terrain 4→1 抽共享光照函数(GLSL+MSL) | 否 | 消债,零观感 | 低,可逆,ctest+像素直通兜 |
| **T1a** | 线性光照工作流 + 角色感知 sRGB 解码/编码 | **否**,双后端现管线可做 | 正确性:消除 gamma 空间光照误差,可延展 | **高**(见 §4 重调) |
| **T1b** | 共享 tonemap 曲线(PBR-Neutral),逐 shader 末端 | 否 | **近 no-op,直到有 >1 headroom** | 低(挂钩) |
| **T2** | HDR float 中间靶 + 补 Metal 离屏路径 + tonemap 移到全屏 pass | **是** | 太阳/太空/夜灯极端动态范围不 clip | **高**(带宽 + Metal 补线) |
| **T3** | auto-exposure | — | 四家全禁用 | **跳过** |

T2 的 float 靶格式选型:`RGBA16F`(Skybolt/Cesium half-float 路线,带 alpha 供合成)vs `R11G11B10F`(移动端带宽减半,无 alpha)——待 T2 立项时定,取决于是否需 alpha 通道合成云/大气。

## 4. 藏起来的成本:gamma 空间手调作废

本项目光照常数(terrain `0.9/0.3/sunTint(1.05,1.0,0.91)`、glTF `0.38/0.62`、各 ambient)**全部在 gamma 空间对着 GE 截图手调**(见 `docs/issues/terrain-visual-maturity-gap-2026-08-02.md`)。T1a 改线性 = **这些常数全作废,须在线性空间逐一重调**,且有回归已匹配的 GE 观感之风险。maplibre/osgEarth 不上线性,恰因 gamma 手调够用且简单。

→ **这是 T1 的真实门槛,不是"免费正确性"**。重调清单(须逐条在钉死场景下重定):
- terrain:`kLambertGain=0.9`、`kShadowFloor=0.3`、`sunTint`、`u_ambient` 注入值
- glTF:`0.38/0.62` 分配、ambient×occlusion、specular 强度
- 夜/太空混合:`exp(elev*8)`、spaceFactor 平滑步端点
- 天空色板 `computeSkyColor` 的 horizonSky/zenithSky(线性下需重定)

## 5. 决策岔口(须裁决)

真正的岔口是**产品野心**,非技术选型:

- **方案 A — 现状够好**:只做 T0(消代码债),光照维持 gamma 手调。承认不干净延展到 PBR/HDR,但对地图引擎"够用"。
- **方案 B — 上台阶到 principled**:做 T1a+T1b(线性 + 角色 sRGB + tonemap 挂钩),接受 §4 全常数重调 + GE 观感重验代价,换可延展地基;terrain/glTF **仍各自着色**(BRDF 不统一),同处一个线性/tonemap 管线。T2/T3 后续按需。

**倾向判断**:若观感目标止步"匹配 GE"、近期无真 PBR 材质 / 昼夜太空极端动态范围需求 → **A(T0 收手)**,因 T1 重调成本换来的正确性已被手调兑现;只要规划含"物理材质 / 电影级昼夜 / 太空" → **B 迟早要还,越晚重调面越大**。此判断留用户定,不代决。

> **✅ 已裁决(2026-08-12):选 B。** 用户确认太阳盘/大气 limb/地平线辉光、水面镜面高光、黄昏/夜间城市灯光、未来 PBR 材质**全部规划要做**——这些正是产生 >1 值、让线性+HDR+tonemap 兑现的内容。B 从"可选债"变为"这些功能的硬前提"。详见 §9 实测与修正后的顺序。

## 6. 若选 B 的分期与落地要点

1. **T0(A/B 共同前置,先落)**:新建 `renderer/TerrainSurfaceLightGLSL.h`,暴露 `kTerrainLightGLSL`/`kTerrainLightMSL` 两语言变体,参数全显式传入(照 `computeSkyColor` 解耦约定);4 处 terrain call site 各切 main 前注入函数、块换调用。glTF **不动**(GLSL/MSL 各 1 份,无字节重复;若走 B 会被重写,现抽再写是白做)。闸门:ctest + **真机像素直通**(shader 运行时在设备编译,host ctest 不验 GLSL/MSL 语法)。
2. **T1a**:在 T0 的共享光照函数内改线性:base 采样后 `srgbToLinear`(仅 color 纹理,heightmap/normal/数据纹理**不碰**——照 Cesium 角色感知),光照全在线性,末端 `linearToSrgb` 编码回 8-bit。§4 常数逐一重调。
3. **T1b**:共享 `tonemap()`(PBR-Neutral)函数进同一头文件,各 shader 末端 `linearToSrgb` 前调用。当前 8-bit 逐 shader(照 Cesium !HDR 路径),留作 T2 移全屏 pass 的接口。
4. **T2(可选,后立项)**:`RenderDevice::Format` 加 float 变体;离屏 FBO 升 half-float;**补 Metal 离屏路径**(MSL 全屏 shader + submit 侧接线,解掉 `RenderDeviceMetal.h:22` 的 `return false`);tonemap 从逐 shader 移到全屏 stage;`OffscreenPostProcess::Effect` 加 `Tonemap`。

## 7. 风险与回归闸门

- **shader 运行时编译**:host ctest 不编译 GLES/MSL 源 → 拆/改 shader 字符串的破坏 ctest 抓不到。真闸门 = 设备跑钉死场景 + 像素 A/B(机制信号 + 像素由用户判,遵 `visual-goal-form-convention`)。
- **T1a 观感回归**:线性重调是全 shader 面改动,非手术式;须钉死场景全套 before/after,逐场景比。回归判据 = GE 匹配度不劣于当前 HEAD。
- **T2 移动端带宽**:half-float 主靶带宽 ×2,tiled GPU 上真成本;须 release 实测帧时(遵 `perf-measured-on-debug-build`),必要时退 `R11G11B10F`。
- **并行会话**:docs/issues 在另一 worktree(`musing-morse-01ac72`)有副本;本文件为仓库根新路径,不碰对方未提交改动。落地改 `Renderer.cpp` 前须 `git status` 划工作面(遵并行会话协作规则 #3)。

## 8. 未决(更新 2026-08-12)

1. ~~A vs B~~ → **已定 B**(§5 裁决 / §9)。
2. **float 靶格式** `RGBA16F` vs `R11G11B10F`:待 T2 立项按移动端带宽 + 是否需 alpha 合成定。
3. **T2 关键路径 = 补 Metal 离屏**(`RenderDeviceMetal.h:22` 现 `return false`)+ HDR 靶,跨双后端;具体改法待 T2 codebase 调研结论(进行中)。

## 9. 实测与修正后的施工顺序(2026-08-12,真机)

**已落地**:
- **T0**(commit `1a939be70`)= terrain 光照 4→1 单一治理点;真机 A/B 判定视觉等价(±1 LSB,与 GPU 启动噪声地板同性质)。
- **T1 P1**(未提交,WIP header)= terrain 切线性(`srgbToLinear`→线性光照→`linearToSrgb`),4 call site 不动,decode/encode 收在共享函数内。
- **T1 P2**(未提交)= 线性重调(`kShadowFloor 0.15`/`kAmbientScale 0.6` **provisional**),luma 分布对齐 gamma 基线。

**真机实测结论(重庆 1500m/45° 冻结场景)**:
- P1 未调线性比 gamma 亮 ~10%(暗部被 L^(1/2.2) 抬起);P2 重调后三分位数对齐 gamma。
- **纯 LDR 下线性观感 ≈ gamma,零可见提升**——为保住认可的观感就得把线性调回 gamma。这印证 §2.3:tonemap/HDR 收益要 >1 值才兑现。
- **地形是漫反射、天生 ≤1,自己不产生 HDR 值** → tonemap/HDR 对地形本身几乎不做事。可见 payoff 落在太阳/limb/水面镜面/夜灯/PBR 上(用户确认全要做)。
- 追"比 gamma 更 punchy"在此俯视影像主导场景是**伪目标**(三版 luma 仅差几单位;`kShadowFloor` 是加在整条 NdotL 斜坡上的常数,压它反而降对比)。

**⚠️ 关键洞察(改施工顺序)——别在 LDR 磨常数**:
上 T2 tonemap 后最终观感由 tonemap 曲线决定,**所有 per-shader 常数须对着 tonemap 输出重调一遍**;P2 在 `linearToSrgb` 下调的值是**一次性丢弃**。更彻底:P1 塞进地形函数的 `linearToSrgb` encode 在 T2 会**被删**(encode 从 per-shader 挪到全屏 tonemap pass)。故 P2/P3 常数只求 good-enough,**唯一真调参在 T2 之后做一次**。

**修正后的关键路径(按依赖 + 避免重复调参)**:
| 序 | 做什么 | 理由 |
|---|---|---|
| 1 | T1 机制铺完(terrain 已 P1;P3 推 glTF/矢量),常数 provisional | 线性是 T2 前提,但不调精 |
| 2 | **T2 地基**:HDR float 靶 + tonemap-on-HDR + **补 Metal 离屏** | 最大单块,所有 payoff 的闸门 |
| 3 | 用**现有天空/大气**做第一个 HDR demonstrator(零新数据,太阳/limb 最亮) | 端到端验 T2 通 + 定 tonemap 操作点 |
| 4 | 对着 tonemap 后管线**调一次**常数(掠视+天空场景) | 唯一真调参 |
| 5 | 逐个上 payoff:水面镜面 → 夜灯 → PBR,复用已验证管线 | 地基已在,每个都便宜 |

真成本在第 2 步(Metal 离屏 + HDR 靶带宽)。

### 9.1 T2 codebase 研究更正(2026-08-12,opus 代理,现读)

**⚠️ 更正上文"补 Metal 离屏是最大单块"——错。** 研究发现离屏 pass 基础设施**两后端都已完整实现**:
- Metal 已实 `createFramebuffer`(RenderDeviceMetal.mm:670-741)/`beginPass`(:798-857)/`endPass`(:859-864);GLES 对应 :581-690/:730-772/:774-778。
- 场景已能画进离屏 FBO(FXAA/AerialFog 开时),Engine.cpp:618-622 已有 `// T2:` `setSceneRenderTarget` 钩子。
- `supportsOffscreenPostProcess()=false`(RenderDeviceMetal.h:22)**唯一**原因 = 无 MSL 全屏 shader(OffscreenPostProcess 只有 GLSL)。补 = ~4 MSL 片元 + 1 顶点 + fullscreen layout 分支(RenderDeviceMetal.mm:415-497 按入口名推断布局,无 fullscreen 布局)。

**真正最大风险 = Metal 场景 PSO 像素格式耦合**(RenderDeviceMetal.mm:633 `colorAttachments[0].pixelFormat=BGRA8Unorm` 建 PSO 时烘死):PSO 为 BGRA8 建、画进 RGBA16F pass → Metal 附件校验失败**静默画不出**,且 GLES 免疫(程序格式无关)→ **策略:整特性 GLES 先落地验证,再移 Metal**。

**依赖序工作项(8 项)**:①Format+FramebufferDesc 加字段 → ②GLES float 路径(+`EXT_color_buffer_half_float` 探测/回落,修 :216-220 空壳恒 true)→ ③Metal float texture(:241-244/:688)→ **④Metal PSO 格式变体(最大风险)** → ⑤场景 shader 写线性 HDR(去 per-shader encode)+ 重调常数 → ⑥`Effect::Tonemap` 作强制终端 → ⑦Metal MSL 全屏 + 翻 flag → ⑧Engine 接线。GLES 切片 = ①②⑤⑥⑧,Metal 移植 = ③④⑦。

**格式定 `RGBA16F`**(非 R11G11B10F):管线存 alpha=1,ES3+Metal 普遍可渲。带宽 ×2 仍是 release 实测闸门。

**tonemap 非可选叠加而是强制终端 encode**:场景一旦画进 HDR 靶,终端 pass 必须 tonemap+encode;`Passthrough`→`Tonemap`,FXAA/AerialFog 在尾部做 tonemap,否则双重编码 / 对未 tonemap 的 HDR luma 做 FXAA。

**reverse-Z / winding 与 HDR 无冲突**(HDR 只碰 color 附件,depth clear/winding 按 pass 设、格式无关)。
