# 架构沉淀:光照 / 天空 / 大气子系统 (environment)

> **架构/方案**文档。质量标尺看 `docs/northstar/lighting.md`(L-V*/L-P* 判据,是本文得失节的唯一权威引用点);行号随重构漂移,以符号名为准。

**规模**:`environment/` ~1.6k 行。

---

## 职责边界

`environment/` 管**时间 → 太阳方向 → 天空/大气渲染**这条单向链,以及它对地表光照的输入(`sunTint`/`ambient`/`clearColor`)。**不管** terrain BRDF 本身(那在 `terrain.md`),只管「颜色进出什么空间、暖度怎么随太阳走、亮部怎么压」。

五个核心类型,均由 `scene/SceneEnvironmentCoordinator` 持有(`unique_ptr`):
- `TimeController`——儒略日时间源
- `SunDirection`(静态解算)——天文太阳方向
- `SkyGradient`(CPU 侧)——解析散射天空色 + 地表 ambient
- `AtmosphereBackgroundPass`(GPU 全屏 pass)——大气散射 + 太阳盘渲染
- `SkyBox`(GPU)——星空/立方体贴图背景

`SceneEnvironmentCoordinator` 本身很薄(44 行),只做 owner + 时间 API 转发 + `sunDirection()` 计算;真正的驱动逻辑在 `scene/SceneFrameStateBuilder.cpp`。

---

## 核心设计决策 + 理由

### 1. time → sun → sky 单向驱动链
每帧由 `SceneFrameStateBuilder::updateEnvironment` 串联:`TimeController::julianDate()` → `SunDirection::compute(jd)` → `Ellipsoid::WGS84().geodeticSurfaceNormal(camera->position())` 求本地天顶 → `SkyGradient::update(sunDir, localUp, camAlt)`。单向、无反馈环。理由:环境状态完全由「现在几点+相机在哪」派生,不需持久状态机,易测(`test_sun_direction`/`test_time_controller` 是唯二有测试的环境代码)。

### 2. 天空渐变模型 —— **两套并存**(L-P1)
- CPU 侧 `SkyGradient::update`:Bruneton-2008 式解析 Rayleigh+Mie+Ozone 散射积分,产出 `zenithColor`/`horizonColor`/`ambientColor`/`sunElevation`,喂 `FrameState.clearR/G/B`(清屏色)与 `FrameState.ambient`(地表补光)。
- GLSL 侧 `computeSkyColor`(`AtmosphereSkyColorGLSL.h`):纯经验色板,喂大气背景 pass 与 aerial fog 两个 shader 共享(注释自称"统一散射的唯一治理点")。
- **设计意图**:GLSL 侧本为让「天空↔雾」不错色差(Cesium `fogColor=groundAtmosphereColor` 思路),已做到——但代价是与 CPU 侧 `SkyGradient` 变成第二套模型,两者互不知情(见得失 L-P1)。

### 3. 大气背景 pass —— 全屏 ray-marching,无 LUT
`AtmosphereBackgroundPass` 端到端移植自 openglobus `Atmosphere.ts`+`atmosphere.frag.glsl`:8-sample 光学深度积分、太阳盘+多层柔光晕、`gl_Position.z=0` 强制远平面绘制在地形之后。`AtmosphereParameters` 保留 LUT 路径可能性但当前是解析法。设计意图是与 openglobus 对齐,不是 Bruneton 论文原版精确复刻。

### 4. 日落着色 —— 天/地/太阳盘三条曲线,两条手抄同步、一条独立(L-P2)
- **量 A**(暖度,膝点 0.30,**须同步、是真债**):`SceneFrameStateBuilder.cpp` 的 `sunLow`(驱动地表 `sunTint`)与 `AtmosphereSkyColorGLSL.h` 的 `smoothstep(0.0,0.30)`(驱动天空底色)是**同一条曲线抄两处**,靠代码注释口头约定同步。
- **量 B**(太阳盘红移/压暗,膝点 0.25,**独立设计,勿并入**):`AtmosphereBackgroundPass.cpp` 的 `sunLowSky`。膝点更早收是刻意的——太阳盘要在更高太阳角就开始红移。
- **地表暖化本体**:`sunTint`(默认 noon `(1.05,1.0,0.91)`)+ `terrainSunAmbient`,在 `TerrainSurfaceLightGLSL.h` 的 `terrainSurfaceLight()` 内消费:`directional = clamp(NdotL*0.9 + kShadowFloor, 0,1)`,`kShadowFloor=0.3`(LDR)= Cesium `vertexShadowDarkness` 同款——即使 `NdotL=0`(完全背光)directional 仍有 0.3 下限,**这是光照对地形 relief 的刻意压平**(避免阴影面纯黑,Cesium 同款设计,非 bug)。

### 5. HDR 线性管线设计意图 T0-T3(L-V4/L-P3/L-V5)
分层来自 2026-08-12 四引擎调研:

| 层 | 内容 | 状态 |
|---|---|---|
| T0 | terrain 光照 4 份复制 → 单一 `terrainSurfaceLight` | ✅ `1a939be70`,真机 A/B 视觉等价 |
| T1a | 线性 + 角色 sRGB(不需 float 靶) | 并入 HDR flag 的 flag-on 变体 |
| T1b | tonemap 挂钩(近 no-op) | 并入 T2 |
| T2 | HDR float 靶(RGBA16F)+ tonemap 终端 + Metal 离屏 | ⚠️ GLES 切片已交付(`dede4688c`),Metal 缺 |
| T3 | auto-exposure | ❌ 明确跳过(四家引擎全禁用) |

**设计意图核心:统一的是颜色管线,不是 BRDF**——四引擎(cesium/skybolt/maplibre/osgearth)调研铁结论是没人统一 terrain 与 glTF 的 BRDF,本项目 terrain 用的正是 cesium 同款 `Lambert*0.9+0.3`。`kEnableHdrPipeline`(`PipelineConfig.h`,`=false`)是**编译期分支开关**,flag-off 逐字节等价现状,flag-on 注入线性变体——两套 shader 源码并存而非运行时分支。

---

## 数据流(关键路径)

```
TimeController.julianDate()
  → SunDirection::compute(jd)
  → localUp = Ellipsoid::WGS84().geodeticSurfaceNormal(camPos)
  → SkyGradient::update(sunDir, localUp, camAlt)
       ├─→ FrameState.clearR/G/B  ← horizonColor()
       └─→ FrameState.ambient     ← ambientColor()
  → sunLow(膝点0.30, 手抄) → FrameState.sunTint / terrainSunAmbient
       → GltfUniformBlock.sunTint (owner=terrain_primitive/instanced 分支写入)
       → TerrainSurfaceLightGLSL.terrainSurfaceLight() 消费, 压平地形 relief

（渲染阶段, SceneRenderPipeline.cpp）
  buildSkyCommands        → SkyBox::buildCommand（order 0）
  buildAtmosphereCommands → AtmosphereBackgroundPass::buildCommand（大气+太阳盘）
       └─ 内部 GLSL computeSkyColor（与 SkyGradient 平行的第二套天空色）
       └─ aerial fog 同调 computeSkyColor（天空↔雾单一治理点, L-V2 已收敛）
```

**要点**:CPU `SkyGradient` 只影响清屏色 + 地表 ambient;GLSL `computeSkyColor` 只影响天空背景像素 + 雾;**两条链语义都叫"天空色",物理上互不读取对方数值**——这正是 L-P1。

---

## 关键契约与不变量

1. **`kEnableHdrPipeline=false` 时必须逐字节等价现状**——T0/T2 硬约束,ctest 是唯一自动化闸门,但**不覆盖 shader 本体**(见 L-P4)。
2. **量 A(暖度 0.30)与量 B(太阳盘 0.25)不可合并**——lighting.md D 节明确判死;膝点不同是设计而非疏漏,强行统一会破坏太阳盘红移时机。
3. **config 穿透验证陷阱**:SDK `EarthSceneConfig` 的 warmth 类字段若默认值 == 成员默认值,"设默认值截图相同"**证明不了链路真的接通**(断链时恰好数值相同)。验证必须设**非默认值**才能证伪。
4. **sunTint 曾死代码复活的教训**(`a5354d3ee`):`5852524af` 把 sunTint 写进 `owner=="surface_tile"` 分支,但地形命令实际 owner 是 `terrain_primitive`/`terrain_instanced`,走另一条分支并 `continue`——下支块永不执行,运行时恒 0。**改 uniform 传递链时必须核对命令真实 `owner` 字符串和 `kind`**;这类死代码在弱观感场景很难肉眼发现,需"临时改 fragColor=uniform 值"反证。
5. **半透明混合空间在 HDR 下改变**:HDR 打开时矢量填充改线性域 alpha 混合,观感偏暗但视为良性——是行为差异点,不是逐字节等价。

---

## 诚实得失

### ✅ 强项
- **天空↔雾已收敛为单一治理点**(`computeSkyColor`,两 shader 共享)——L-V2 达成。
- **terrain 光照四份复制已统一为单一 `terrainSurfaceLight`**(T0),真机 A/B 视觉等价(唯一差异 ±1 LSB GPU 重编噪声)。
- **HDR 管线默认关时对生产路径零回归**,flag-on 变体已端到端跑通(GLES,真机 HDRDIAG 信号)。

### ⚠️ 短板 / 已知债(逐条对应 lighting.md L-P1~L-P4)
- **L-P1 同一片天空两套色模型**(未修):CPU `SkyGradient`(解析散射,喂 clear/ambient)与 GLSL `computeSkyColor`(经验色板,喂天空背景+雾)语义都是"天空色"却互不知情。修法方向明确,但 SkyGradient 有解析散射语义,合并前须先判语义兼容性——未做。
- **L-P2 日落膝点曲线手抄两处**(未修):量 A(暖度 0.30)在两处各抄一份,靠注释口头同步,无机制保证——漏改一处会致天地暖度错位。
- **L-P3 HDR 线性管线半成品,冻结挂起**(L-V5 已拍板:长期做、短期挂起):三缺口——①`kEnableHdrPipeline=false` 默认关 ②Metal `supportsOffscreenPostProcess()` 恒 false,`Tonemap`/`AerialFogTonemap` 等均无 MSL 入口 ③provisional 常数未对 tonemap 标定(`kSunHdrBoost=6.0`、`kShadowFloor=0.15`/`kAmbientScale=0.6`)。**挂起期真风险=静默腐烂**:半成品+默认关+零守卫,并行改 shader 可能悄改坏 HDR 开启态而无人察觉(编不到、跑不到、测不到)。守卫方案(结构断言/glslc 守卫/仅记录)待用户定,当前"仅记录"。
- **L-P4 天空/雾/tonemap 全链零测试**(未修):`tests/unit/environment/` 仅 3 文件,`computeSkyColor`/`AtmosphereBackgroundPass`/`OffscreenPostProcess`/`Tonemap`/`AerialFog` 在 tests/ 命中 **0**。根因:这些 shader 是 C++ 字符串,运行时才编译,host ctest 抓不到语法/语义错误。可行网兜:把 `computeSkyColor` 纯数值逻辑抽成可 host 编译的 C++ 对拍,至少给 L-P2 曲线一致性上一道单测——尚未做。
- **HDR payoff 尚未证明**:纯 LDR 下线性观感 ≈ gamma 零可见提升;payoff 全在太阳盘/水面镜面/夜灯/PBR 金属这些能 >1 的发光源,而这些内容目前均**未建成**(lighting.md B 节 payoff 前置清单四项全 ❌/⚠️)。管线是必要不充分条件。

---

## 扩展点
- **加新天空模型**:唯一正确切入点是 `computeSkyColor`——但目前只覆盖 GLSL 侧;若要连带影响 CPU `clearColor`/`ambient`,仍要在 `SceneFrameStateBuilder::updateEnvironment` 手动同步(L-P1 未解前两处独立改)。
- **加新大气效果**(云层/极光):作为 `AtmosphereBackgroundPass` 的新 compose 分支或独立 pass,参照 `composeAtmosphereOutput` LDR/HDR 双变体注入范式(编译期择一,非运行时分支)。
- **统一 BRDF**:lighting.md D 节已判死——四引擎调研结论是没人统一 terrain 与 glTF 表面模型,该切入点**不存在、也不该开**。真正该做的是颜色管线(HDR/tonemap/线性化)统一。
- **HDR 唤醒**:切入点是「挑一个发光内容 + 翻 `kEnableHdrPipeline=true` + 调常数 + 补 Metal MSL 入口」,四选一起步。

---

## 对照系
- **大气/天空模型**:`AtmosphereBackgroundPass`/`SkyBox`/`SunDirection`/`AtmosphereParameters` 全部端到端移植自 openglobus——是**自研移植**非自主设计(Architecture Overview 定位"环境/大气子系统对齐 openglobus")。
- **地表光照公式**:`kLambertGain=0.9`/`kShadowFloor=0.3` 显式对齐 Cesium `lambertDiffuseMultiplier`/`vertexShadowDarkness`。
- **tonemap 策略**:对齐 Cesium 默认(PBR-Neutral、曝光固定、无 auto-exposure)。
- **CPU `SkyGradient` 散射模型**具体对齐哪个引擎/论文——**未验证**,留待专项。
- **铁结论**:**该统一的是颜色管线,不是 BRDF**——L-P1 两套天空模型未合并恰是"颜色管线尚未统一"的具体体现,与"BRDF 不需要统一"是两件不矛盾的事。
