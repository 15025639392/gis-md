# 光照 / 颜色管线模块北极星 — 产品体验判据

**这份文档回答「做到什么程度算好、现在到哪了」。**
不回答「代码在哪」(那是 `AI_INDEX.md`),也不回答「当时怎么修的」(那是 `docs/issues/*`)。
本文是活的,随每次专项收官更新。

覆盖范围:天空底色 / 大气背景 / aerial fog / 日落着色(地表+天空+太阳盘)/
HDR 线性管线 + tonemap。**不含** terrain BRDF 本身(那在 `terrain.md`)——本文只管
「颜色进出什么空间、暖度怎么随太阳走、亮部怎么压」。

## 怎么用

- **判据编号(L-V1…)是稳定引用锚点。** 你说「L-V4 我不满意」即指该条,不必重描述观感。
- **状态**:✅ 达成(有证据) ⚠️ 部分达成/有已知缺口 ❌ 未做 🔒 待你拍板
- **类型决定谁说了算**:【机制】我自证(命令/计数/测试红绿);【观感】像素判断**归你**。
- **债编号(L-P1…)** 同为引用锚点,见 C 节。这里的债多是**结构债**(重复/缺测/半成品),
  不全是性能债——沿用 `vector.md` C.5 的宽口径。
- **`推断` 标记**:带此标记的判据是我从现状推的,不是你说过的,优先请你校对。

**命名空间**:本文用 `L-` 前缀(`L-V*` 判据 / `L-P*` 债)。理由见项目 `CLAUDE.md`——
无前缀会让「V3」跨模块(vector 的 V3)歧义。

---

## 北极星一句话

> 日落时**天、地、太阳盘三者暖度同步**推进,不错位;亮部(太阳盘/辉光/未来的水面镜面
> 与夜灯)能超出显示范围而被 tonemap 优雅压回,而非硬切成白块或糊成钝灰。
> 颜色**进出空间单一治理、不靠多套模型凑近似**。

**现状定位(2026-08-16)**:LDR(默认生产路径)下日落暖化已生效(L-V1);HDR 线性管线
内容已补齐但**默认关、仅 GLES、常数未调**(L-V4),ship 与否待拍板(L-V5)。
真正的短板不是「缺效果」,是**同一件事写了多套、且零测试兜底**(L-P1~L-P4)。

---

## A. 体验判据(L-V)

| # | 判据 | 类型 | 状态 | 代价 | 证据 / 差距 |
|---|---|---|---|---|---|
| **L-V1** | 日落地表暖化:太阳贴地平线时受光面转暖橙、阴影面弱暖补光 | 观感 | ✅ | GltfUniformBlock 加 `sunTint`(3 float)+ 复用 `ambient`,GPU ~0 | **B1 复活(`a5354d3ee`)**:原 `sunTint` 自 5852524af 起是死代码(owner 是 `terrain_primitive` 非 `surface_tile`,块里没 `u_sunTint`);经 gltf 块接通后**LDR 出厂即生效**。曲线膝点 0.30(见 L-P2)。像素观感待你拍板 |
| **L-V2** | 天空↔雾无缝、无色差:雾霾色=该视线方向的天空色,逐分量恒等 | 机制 | ✅ | 单函数 `computeSkyColor`,两 shader 共享 | 大气背景 pass 与 aerial fog **同调**一个 `computeSkyColor`([AtmosphereSkyColorGLSL.h](../../scaffold/src/earth_engine/environment/AtmosphereSkyColorGLSL.h));天空↔雾这对已收敛为单一治理点(Cesium `fogColor=groundAtmosphereColor` 思路)。⚠️ 但它与 CPU 侧 `SkyGradient` 仍是两套,见 **L-P1** |
| **L-V3** | HDR×雾共存:开 HDR 时地平线不再硬切(单效果槽互斥已解) | 机制 | ⚠️ | RGBA16F 靶 + `AerialFogTonemap` 合并终端 | **B0(`64b601e79`)**:HDR 下把 fog 数学 + PBR-Neutral tonemap 合进一个终端 pass,消硬地平线。⚠️ `fogColor` 须 `srgbToLinear` 后再混(初版漏致地平线暖带过亮,已修入同 commit)。**仅 HDR 路径生效**,LDR 默认不走此路 |
| **L-V4** | HDR 下**全内容**输出线性正确(地形/天空/雾/矢量/标签/图标/glTF 均不洗白) | 机制 | ⚠️ | 编译期 flag,LDR 零回归 | **B2 内容完整里程碑**:刀1 无光照内容(`301e56006`,`encodeSceneOutput` 包 srgbToLinear)+ 刀2 glTF 真线性 PBR(`0fb60eb5d`,`hdrAlbedo` 解 albedo/emissive)。**缺口**:①默认关(`kEnableHdrPipeline=false`)②仅 GLES,Metal 无 MSL 入口 ③常数未对 tonemap 重调(L-P3)。均属 **L-P3** 债 |
| **L-V5** | HDR 线性管线是否作为默认 ship | 🔒→✅ | 移动端已 GPU-bound,RGBA16F 带宽翻倍 | **已决(2026-08-16,用户拍板):长期路线内,短期挂起。** 短期无发光内容 → 开 HDR 白天零提升(两次真机证);长期要做发光内容(见 B 节前置清单)→ HDR 是前提,届时点亮。**推论**:B0/B2/T2 不是"待决",是"冻结待唤醒",按 L-P3 处置。B1 已随 LDR ship(bug 修复,与 HDR 无关) |
| **L-V6** | 日落时天、地、太阳盘暖度**同步**推进,不错位 | 观感 | ⚠️ | — | 天(L-V2 内)与地(L-V1)共享 0.30 膝点曲线**但手抄两份**、靠注释口头同步;太阳盘另有独立 0.25 ramp(设计如此)。**同步性目前靠人肉维护,无机制保证**,见 **L-P2** |

---

## B. HDR 分层地图(设计基线,来自 2026-08-12 四引擎调研)

> 铁结论:**没人统一 BRDF**(cesium/skybolt/maplibre/osgearth 全部 terrain 与 glTF
> 表面模型分离)。「统一光照=一条公式」方向**作废**,该统一的是**颜色管线**不是 BRDF。
> tonemap 曲线与线性工作流**不依赖 HDR float 靶**(cesium 8-bit 路径也逐 shader 调
> PBR-Neutral)。cesium 默认 tonemapper=**PBR-Neutral**,曝光固定,auto-exposure 四家全禁用。

| 层 | 内容 | 状态 |
|---|---|---|
| T0 | terrain 光照 4 份复制 → 单一 `terrainSurfaceLight` | ✅ `1a939be70`(真机 A/B 视觉等价,仅 ±1 LSB 重编噪声) |
| T1a | 线性 + 角色 sRGB(无需 float 靶) | 并入 HDR flag 的 flag-on 变体 |
| T1b | tonemap 挂钩(近 no-op 直到有 >1 headroom) | 并入 T2 |
| T2 | HDR float 靶 + tonemap 终端 + 补 Metal 离屏 | ⚠️ GLES 切片 `dede4688c`,Metal 缺(L-P3) |
| T3 | auto-exposure | ❌ 跳过(四家全禁用) |

**⚠️ 铁律(两次真机实验证)**:纯 LDR 下线性观感 ≈ gamma 零可见提升;地形漫反射
天生 ≤1 不产 HDR 值,payoff 全在**太阳盘/水面/夜灯/PBR**这些能 >1 的发光源。
**管线是必要不充分**——payoff 要逐个把发光源建成线性 HDR 才显得出来。

### HDR payoff 前置清单(唤醒 HDR 的真正内容,长期做)

> **依赖方向**:不是 HDR 依赖这些,是**这些依赖 HDR**。做其中任一个才第一次能证明
> HDR 值得开(L-V5)。唤醒 HDR = 挑一个发光内容 + 翻 flag + 调常数(L-P3 刀3)+ 补 Metal(刀4)。

| 发光内容 | 为什么非 HDR 不可 | 状态 |
|---|---|---|
| 太阳盘过曝晕开 | 芯部要 >1 才能 tonemap 成白芯+暖边,LDR 只能 clamp 成白饼 | ❌ 未做(B0 只解决了雾×tonemap 互斥,没做过曝) |
| 水面镜面高光 | 镜面瞬时可达几十倍亮度 | ❌ 未做 |
| 夜景灯火 | 暗背景里的点光源要"刺出来" | ❌ 未做 |
| PBR 金属材质 | 金属高光同理 >1 | ⚠️ glTF 线性 PBR 地基有(B2 刀2),但没有 >1 的发光材质内容 |

---

## C. 技术债 / 结构债(明记,不假装没有)

| # | 债 | 影响 | 现状 |
|---|---|---|---|
| **L-P1** | **同一片天空两套色模型**:CPU 侧 `SkyGradient`(解析散射)算 `horizonColor`/`ambientColor`,喂 `frameState.clearR`(清屏色)+ `frameState.ambient`(地表补光)([SceneFrameStateBuilder.cpp:66-71](../../scaffold/src/earth_engine/scene/SceneFrameStateBuilder.cpp:66));GLSL 侧 `computeSkyColor` 是另一套经验色板(horizon `(0.68,0.79,0.86)`/zenith `(0.06,0.24,0.55)`),喂天空背景 + 雾([AtmosphereSkyColorGLSL.h:30-35](../../scaffold/src/earth_engine/environment/AtmosphereSkyColorGLSL.h:30)) | 用户看到的天空来自 GLSL,地表补光/清屏底色来自 CPU;改一套不动另一套,数值漂移 → 「雾色与天空对不上」「地物受光与天空氛围不配套」 | **未修**。注意:天空↔雾这对**子债已收敛**(computeSkyColor 单一治理点,L-V2)。剩余分叉是 **CPU SkyGradient ↔ GLSL computeSkyColor**。修法方向=让 clear/ambient 也从 computeSkyColor 采样(或反向),但 SkyGradient 有解析散射语义、不是纯色板,合并要先判语义是否兼容 |
| **L-P2** | **日落膝点曲线:一条暖度曲线手抄两处 + 太阳盘另有独立 ramp**(修正版,勿再按「三份抄错」理解) | 漏改一处 → 天地暖度错位;错误统一 → 破坏太阳盘红移时机 | **未修**。**量 A(暖度,膝点 0.30,须同步、真债)**:①[SceneFrameStateBuilder.cpp:79](../../scaffold/src/earth_engine/scene/SceneFrameStateBuilder.cpp:79) `t=e/0.30`+`sunLow=1-t²(3-2t)`(驱动地表 sunTint) ②[AtmosphereSkyColorGLSL.h:47](../../scaffold/src/earth_engine/environment/AtmosphereSkyColorGLSL.h:47) `smoothstep(0.0,0.30)`(驱动天空底色)——同一条曲线抄两份,靠注释口头约定同步。**量 B(太阳盘红移/压暗,膝点 0.25,独立设计,勿并入)**:[AtmosphereBackgroundPass.cpp:249](../../scaffold/src/earth_engine/environment/AtmosphereBackgroundPass.cpp:249) `sunLowSky=1-smoothstep(0.0,0.25)`——膝点更早收是刻意的(太阳盘要在更高太阳角就开始红移)。**正确修法=只把 A 两处抽成单一 `sunsetWarmthRamp(sunElev)` 供 CPU/GLSL 共用;B 原样保留并标注「独立量勿并入」** |
| **L-P3** | **HDR 线性管线半成品(冻结挂起)**:默认关 + 仅 GLES + 常数未调 | **⚠️ 挂起期的真风险=静默腐烂**:半成品 + 默认关 + 零守卫,并行 churn 改 shader 会悄悄改坏 HDR 开启态而无人察觉(它编不到、跑不到、测不到) | **L-V5 决:冻结待唤醒(长期做)**。现状三缺口:①`kEnableHdrPipeline=false`([PipelineConfig.h:21](../../scaffold/src/earth_engine/renderer/PipelineConfig.h:21)) ②Metal `supportsOffscreenPostProcess(){return false;}`([RenderDeviceMetal.h:22](../../scaffold/src/earth_engine/platform/ios/RenderDeviceMetal.h:22))——`Tonemap`/`AerialFogTonemap`/`encodeSceneOutput`/`hdrAlbedo` 无 MSL 入口 ③provisional 常数 `kSunHdrBoost=6.0`([AtmosphereBackgroundPass.cpp:85](../../scaffold/src/earth_engine/environment/AtmosphereBackgroundPass.cpp:85))、`kShadowFloor=0.15`/`kAmbientScale=0.6`([TerrainSurfaceLightGLSL.h:86-87](../../scaffold/src/earth_engine/renderer/TerrainSurfaceLightGLSL.h:86))未对 tonemap 标定。⚠️ Metal PSO 像素格式烘死是最大风险,故 GLES 先行。**⚠️⚠️ 防腐守卫现状=零**:host ctest 不编译 shader,`glslc` 本机缺失 → HDR 开启态从没被任何自动化碰过。唤醒前的现状:**裸挂,churn 会腐烂**(守卫方案待定,见文末「HDR 挂起处置」) |
| **L-P4** | **天空/雾/tonemap 全链零测试** | 复制多份 + 无网兜,改错只能真机肉眼发现 | **未修**。`tests/unit/environment/` 仅 3 个:`test_sun_direction` / `test_time_controller` / `test_atmosphere_scattering`(后者只测 CPU 侧 **`SkyGradient`**);grep `computeSkyColor`/`AtmosphereBackgroundPass`/`OffscreenPostProcess`/`Tonemap`/`AerialFog` 在 `tests/` 命中 **0**。根因=这些 shader 是 C++ 字符串**运行时才编译**,host ctest 不编译 GLES/MSL 源、抓不到。**可行网兜**:把 `computeSkyColor` 的暖度/膝点等纯数值逻辑抽成可 host 编译的 C++(与 GLSL 共享或对拍),至少给 L-P2 的曲线一致性上一道单测 |

---

## D. 已判死 / 勿再提(边界)

| 方案 | 死因 |
|---|---|
| 统一 terrain 与 glTF 的 BRDF 成一条公式 | 四引擎调研:**没人这么做**,cesium globe 正是 `Lambert*0.9+0.3`,该统一的是颜色管线不是 BRDF |
| auto-exposure(自动曝光) | cesium/skybolt/maplibre/osgearth 四家**全禁用**,曝光固定 |
| 把 L-P2 的太阳盘 ramp(0.25)并入暖度曲线(0.30) | 太阳盘红移要在更高太阳角就开始,膝点不同是设计;强行统一破坏红移时机 |
| 在纯 LDR 下磨 HDR 常数 / 追「比 gamma 更 punchy」 | 实测 LDR 下线性≈gamma 零提升;常数要对着 tonemap 输出调,LDR 下调了 T2 还要全丢 |

---

## HDR 挂起处置(L-V5 决后,2026-08-16)

L-V5 决"长期做、短期挂起"后,B0/B2/T2 = **冻结待唤醒**。挂起态的唯一敌人是
**静默腐烂**(见 L-P3):默认关 + 零守卫,并行 churn 会悄悄改坏 HDR 开启态。

**⚠️ glslc 本机缺失**(`which glslc` 空),把"能不能编过"的守卫降级——host 无 GLSL
编译器,任何 host 守卫只能查**字符串结构完整**(删除/重命名/分支坍缩),查不了 GLSL
语法错。真正的编译校验仍须设备或装了 glslc 的 CI。

守卫三选一(**待用户定**,涉及构建/CI 基础设施):

| 方案 | 覆盖 | 成本 | 谁的决策 |
|---|---|---|---|
| **A 结构守卫**(host ctest 断言 HDR 变体:助手函数已注入 / ON≠OFF / 单一 main) | 删除、重命名锚点、分支坍缩 | 小(需把注入器/常量暴露给测试,含轻量重构) | 我可做 |
| **B glslc 编译守卫**(装 shaderc/glslang,ctest 真编两变体) | A + GLSL 语法错 | 中(装工具链 + 过滤 ES 假阳性) | 需你同意加构建依赖 |
| **C 仅记录**(不上守卫,北极星白纸黑字标"裸挂会腐烂") | 无(靠唤醒时重验) | 零 | 已在 L-P3 记 |

**我的推荐**:A(结构守卫)——churn 期最常发生的是"共享 shader 被改致锚点失效 →
HDR 变体引用未定义函数",这正是 A 能抓、且不需要任何工具链的失败模式。语法错这类
低频风险留给唤醒时的真机验证。**现状=C 已落地**(本文即记录),A/B 待定。

---

## 附:关联锚点

- 设计文档:`docs/issues/lighting-color-pipeline-architecture-2026-08-12.md`(仓库根 docs)
- 相邻模块:地形 BRDF/质感 → `terrain.md`;影像对齐/GCJ → `imagery.md`
- 记忆:`lighting-color-pipeline-survey-2026-08-12`(四引擎调研全程)、
  `sunset-terrain-shading-2026-08-12`(B0/B1/B2 收官)
