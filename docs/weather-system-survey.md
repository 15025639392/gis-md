# 天气系统（视觉特效）选型调研

范围：云 / 降水 / 天气雾 等**渲染表现**。真实气象数据图层（雷达回波、风场时序）与天气↔相机/调度耦合不在本文，另议。

调研日期 2026-08-09。参考源码本地位于 `.ref/`：`cesium-js@26.1.0`、`osgearth`、`maplibre-gl-js`、新拉 `Skybolt`（MPL-2.0）、`TileableVolumeNoise`（sebh）。

---

## 0. 结论

**推荐 C 案：半分辨率射线步进的分层体积云（Skybolt 简化版）+ 全球云量覆盖图驱动 + 离线烘焙 tileable 3D 噪声。**降水走独立的两件套（视锥内 instanced 粒子 + 屏幕空间雨幕），地面湿/雪走材质侧。

排序理由（按"哪个最对"而非工程量）：

1. 球面尺度上**只有射线步进成立**。Cesium 的 `CloudCollection` 是 billboard 积云，本质是装饰物件，没有"飞进云里""云下压城"的视差与遮挡；2D 云贴图叠球更是俯视才成立，掠视立刻穿帮。我们的相机覆盖 0m→太空全程，掠视是常态视角。
2. C 案的增量成本大头**不在云本身，而在三项长期资产**：Metal 侧后处理接线、3D 纹理与半浮点格式进 `RenderDevice` 抽象、时域重投影框架。这三项日后做任何屏幕空间效果（TAA、SSAO、体积雾、体积光）都要付，先付不浪费。
3. 真正的一次性成本只有 raymarch shader 两份（GLSL/MSL）。

**先决阻塞项（必须排在云之前）**：`OffscreenPostProcess::initialize` 在 Metal 后端直接返回 `false`（`src/earth_engine/renderer/OffscreenPostProcess.h:62` 注释自陈"全屏 shader 仅有 GLSL，MSL 接线待补"）。不补它，天气系统天生只有 Android 有。

---

## 1. 引擎侧基线（实测，非回忆）

| 事实 | 位置 | 对天气的含义 |
|---|---|---|
| 全仓 `grep -ilE "weather\|cloud\|rain\|snow\|precip"` 命中 **0** | `src/` | 全新子系统，无历史包袱也无可复用件 |
| 大气散射已有：解析 Rayleigh+Mie+Ozone 全屏 pass，16 样本光学深度，含 sun disk + bloom，**不依赖 LUT** | `environment/AtmosphereBackgroundPass.*`（406+55 行）、`AtmosphereParameters.h` | 云的入射光照（sunIrradiance/skyIrradiance）可直接从这套取，不必另建 |
| SkyBox / SkyGradient / SunDirection / TimeController(儒略日) 齐备，共 1559 行 | `environment/` | 天气状态可挂在 `SceneEnvironmentCoordinator` 下，接线位置现成 |
| aerial fog 已在后处理里，逐像素按视线方向算天空色作雾色 | `OffscreenPostProcess::Effect::AerialFog` | ⚠️ 云与它会**双重雾化**同一段大气，需统一到一处消光 |
| 离屏后处理 **Metal 返回 false** | `OffscreenPostProcess.h:62` | 先决阻塞项 |
| 地形深度 prepass 已有，**半分辨率**（`kResolutionDivisor = 2`），产可采样深度纹理 | `renderer/TerrainDepthPrepass.h` | 云做地形遮挡的深度源现成；⚠️ 它只含地形，不含矢量/模型 |
| GLES **3.0**（`#include <GLES3/gl3.h>`） | `platform/android/RenderDeviceGLES.cpp:7` | 有 `sampler3D`、MRT、instancing；**无 compute shader** → 噪声只能离线烘焙或用 fragment pass 烘 |
| `TextureDesc::Format` 只有 `RGBA8/RGB8/R8/Depth32F`，无 3D、无浮点 | `renderer/RenderDevice.h:191` | 需扩抽象（3D + R16F/R11F），或退而用 2D 切片图集手动 trilinear（Cesium 的做法） |
| `TextureDesc::arrayLayers` 已支持 texture2DArray | 同上 :189 | 3D 噪声可先以 array 承载，插值手写 |
| `FramebufferDesc` 只有单 color 附件（`maxDrawBuffers()` 存在但未用） | 同上 :213 | Skybolt 的 color+cloudDepth 双输出需扩 MRT，或拆两 pass |
| reverse-Z 深度约定 | `renderer/DepthConvention.h` | 云的深度重建与 fog 同一条公式，两处必须一致（`Renderer.cpp:2062` 已有同类告诫） |
| 支持 instancing、有 SDF billboard 符号路径（36B 顶点） | `supportsInstancing()`、`renderer/SymbolShape.h` | 降水粒子可复用这条路，不必新建渲染器 |

---

## 2. 参考项目横评

| 项目 | 云的做法 | 数据来源 | 可搬性 |
|---|---|---|---|
| **Cesium 26.1** | `CloudCollection`（1041 行）+ `CumulusCloud`：billboard quad，FS 内对椭球 SDF 做小步进，噪声用运行时 fragment pass 烘的 2D 切片图集（`CloudNoiseFS`，`_noiseTextureRows=4`）。**无全球云层、无云影、无穿云** | 应用层逐朵摆放 | 只适合"局部装饰云"；**球面天气系统不可用** |
| **Cesium `Fog.js`** | 距离雾 + 用雾密度**反过来剔除地形**（`density=0.0006`，`heightScalar`，`maxHeight=800km`） | — | ⚠️ 值得单独抄的一点：雾密度当加载调度信号（`renderable=false` 仍用于剔除）。这属"天气↔调度耦合"，本次范围外 |
| **Skybolt**（MPL-2.0，OSG+GLSL） | `VolumeClouds.frag` 488 行：行星尺度分层积云（cloudLayer 1000–7000m，4 类云各自 bottom/top/density/noise 权重），主步进 `initialStepSize=100m`、`maxStepSize=500m`、`growth=1.002`、`maxRenderDistance=300km`、`iterations=1000` 上限；每步 6 个光照样本（`RANDOM_VECTORS` 抖动 + 指数距离 1/2/4/8/16/32×200m）算透射；powder 项 + HG 相函数 + 假多次散射（`scatterDistanceMultiplier=0.5`）；相机高度**显式三分支**（云下 / 云中 / 云上，云上还有地平线 5px 抗锯齿淡出） | 覆盖 = NASA Blue Marble Clouds 8192 全球图（`cloud_combined_README.txt`，可商用）+ detail 图；3D 噪声离线/启动期生成（`CloudNoiseTextureGenerator`） | **最贴我们场景的参考**，直接对标"整个地球 + 相机可到任意高度"。参数需大幅下调 |
| Skybolt 时域上采样 | `CloudsTemporalUpscaling.frag`：4×4 Bayer，**每帧只算 1/16 像素**，其余从上一帧重投影 + 8 邻域 clamp + Catmull-Rom 采样；云自带一张 `R32F` 云深度用于重投影 | — | 机制可搬，但 `R32F` 与 MRT 我们都得先补 |
| Skybolt `BillboardCloud.*` | 远景/低配回落用 billboard | — | 可作为低端机降级档 |
| **osgEarth** | 本体只有 `Sky`/`Ephemeris`，**云靠商业 SDK**（SilverLining；海面靠 Triton） | — | 佐证：开源地理引擎里"认真做云"的都外挂商业件，Skybolt 是少数自研且开源的 |
| **maplibre-gl-js** | 无云。`cloud` 命中全是无关词 | — | 无参考价值 |

---

## 3. 方案对照

四个候选，按"云"这一项：

| | A 2D 云图叠球 | B billboard 积云（Cesium 式） | **C 半分辨率分层体积云（推荐）** | D 全分辨率 + 完整时域重投影（Skybolt 原样） |
|---|---|---|---|---|
| 做法 | 全球云量图当一层半透明 raster overlay 贴在云高球壳上 | 应用层摆 N 朵 billboard，FS 内小步进 | 全屏（1/4 分辨率）射线步进 1–2 层云壳，coverage 图 + 3D 噪声，6→3 光照样本 | C + 4×4 Bayer 1/16 像素 + 重投影 + 云深度 MRT |
| 掠视/穿云 | ❌ 俯视勉强，掠视全穿帮 | ❌ 无云层概念 | ✅ 三态（云下/云中/云上）成立 | ✅ |
| 云影落地 | 可（同一张图投影） | ❌ | ✅（复用 coverage 图，便宜） | ✅ |
| 相机高度覆盖 | 仅高空 | 仅局部近景 | 0m→太空全程 | 全程 |
| GPU 成本（估，1080p 中高端 Android，**必须 PoC 实测**） | ~0.2ms | 与朵数线性，装饰量级 | **~2–4ms**（1/4 分辨率 ≈ 13 万像素 × 约 24 步 × 约 3 光样 ≈ 50–80 次采样/像素） | ~0.5–1ms，但**多一帧延迟 + ghosting 调试成本** |
| 内存 | 云图 8192² R8 ≈ 64MB（可切瓦片/降到 4096²=16MB） | 噪声图集 ~1MB | coverage 4096² R8 16MB + 3D 噪声 128³ R8 2MB + detail 512² 0.25MB | 同 C + 2×历史帧 RT |
| 需要的抽象改造 | 无（复用现有 raster overlay 通道） | 无 | **3D 纹理 + R16F 进 `TextureDesc`**；Metal 后处理接线 | C 的全部 + MRT + 历史 RT 管理 + 重投影矩阵 |
| 低端机降级 | 本身就是降级档 | — | 降到 1/8 分辨率或退 A 案 | 退 C |
| 主要风险 | 观感天花板低，做了还得重做 | 与地球尺度语义冲突 | 1/4 分辨率的**边缘锯齿/闪烁**；与 FXAA 顺序 | ghosting、快速转动时重建失败；调试面大 |
| 工程量（AI 协作基准） | 0.5 天 | 1 天 | **3–4 天**（含抽象改造 2 天） | C + 2–3 天 |

**为什么不推 D**：时域重投影是纯性能优化，它换来的 2–3ms 在我们还没测出 C 案真实开销之前是投机。C 的 shader 与 D 完全兼容——D 是在 C 之上加一层 pass，不是另起炉灶。先 C 后按实测决定要不要 D，这不是"MVP 后重构"，是同一架构的两个档位。

**为什么 A 不是"先做简单的"**：A 与 C 的数据资产（全球 coverage 图 + 采样函数）完全共享，A 天然是 C 的低端降级档。所以 A 可以做，但要以"C 的 fallback 分支"身份做，而不是以"第一版"身份做。

### 降水（独立于云的三件套）

| 层 | 做法 | 成本 | 备注 |
|---|---|---|---|
| 视锥内粒子 | instanced 拉伸 quad，绕相机的固定盒内 wrap 循环（不做世界坐标粒子池），速度=风场+重力 | 低，复用 `supportsInstancing()` | 只在相机低于云底且 precipitation>0 时开 |
| 屏幕空间雨幕/雪幕 | 全屏 pass，滚动噪声 + 视线方向偏移 | ~0.3ms | 给"密度感"，粒子给"个体感"，两者叠加才像 |
| 地面响应 | 湿地面（raster overlay 压暗 + 提高镜面）/ 雪盖（按高程与坡度混白） | 近 0（着色器分支） | **性价比最高的一项**，比云更能让人相信"在下雨" |

**⚠️ 顺序建议**：地面响应可以先于云做，它不依赖任何抽象改造，且独立成立。

---

## 4. 推荐路线（每期带验收信号）

遵循项目既有约定：观感判断归用户，我方只给机制信号；性能一律 release；相机场景钉死。

| 期 | 内容 | 机制信号（"达成 vs 未达成"二元） |
|---|---|---|
| **P0 先决** | Metal 侧 `OffscreenPostProcess` MSL 接线；`TextureDesc` 加 3D + R16F/R11F；`FramebufferDesc` 加 MRT | iOS 上 `Effect::Passthrough` 像素与直绘一致（现成的冒烟验证器）；3D 纹理上传后采样单测通过 |
| **P1 天气状态层** | `WeatherState`（cloudCoverage / cloudBase / cloudTop / precipType / precipIntensity / windVec）挂 `SceneEnvironmentCoordinator`；先只驱动 aerial fog 密度与地面湿/雪 | EnvSnap 新增 weather= 行；改 coverage 值时 fog 密度与地面材质读数按曲线变化 |
| **P2 体积云 C 案** | 3D 噪声离线烘焙（`TileableVolumeNoise` 生成）；coverage 用 Blue Marble 4096²；1/4 分辨率 raymarch pass，插在场景 pass 后、aerial fog 前；三态高度分支 | `cloudPass` 耗时（release，钉死三个相机：地面掠视 / 云中 / 太空俯视）；平均步进数；早退命中率；三态切换处**无跳变**（连续帧亮度差有上界） |
| **P3 云影 + 降水** | `CloudShadows` 复用 coverage 图打进地形着色；粒子 + 雨幕 | 云影开关的地形亮度差非零且与 coverage 相关；粒子数上限不随相机移动漂移 |
| **P4 视情况** | 时域重投影（D 案）、低端机降级到 A 分支 | 1/16 采样下 `cloudPass` 降幅；快速转动时重建失败像素占比 |

---

## 5. 已知坑（提前记）

1. **双重雾化**：云的消光与 aerial fog 覆盖同一段大气，各算各的会偏暗。要么云 pass 内自带 aerial perspective 后 fog 跳过云区，要么 fog 只作用于 cloudDepth 之外。Skybolt 的做法是云 pass 内直接调 `GetSunAndSkyIrradiance` 与大气同源——我们的大气是解析近似，同源这条同样成立（`AtmosphereSkyColorGLSL.h` 已经是共享 GLSL 片段，正是为这种复用准备的）。
2. **半分辨率深度只含地形**：`TerrainDepthPrepass` 抽的是地形命令，矢量/模型不在内。云拿它做遮挡会把建筑/符号后面的云画出来。要么接受（云在远处，误差小），要么改成通用场景深度——后者影响 T2 符号遮挡，需一起评估。
3. **FXAA 与云的顺序**：云若在 FXAA 之前，1/4 分辨率的云边会被 FXAA 当边缘继续糊；若在之后，云边自身锯齿无人处理。倾向云 pass 内自带边缘 smoothstep（Skybolt 的 `antiAliasSmoothStep`），并放在 FXAA 之前。
4. **相机三态的过渡**：Skybolt 显式写了三个分支且各自 rayNear/rayFar 求法不同，穿越边界（1000m / 7000m）时若不做重叠区淡化会跳变。我们相机是连续飞行，这条必踩。
5. **coverage 图的极地与接缝**：等距柱状投影的全球云图在极点会被极度拉伸，U 从 1 绕回 0 处 LOD 有奇点——Skybolt 专门写了"两次 `textureQueryLod` 取 min"的绕法并注明未完全消除。我们地形侧已有极帽渲染器（`PolarCapRenderer`），云图的极地处理要对齐同一套语义。
6. **`static_assert` / 机制信号纪律**：新加的诊断字段要 `strings` 验证真进了 `.so`（本项目已踩两次）。
7. **不要用 debug 构建评估云的开销**：raymarch 是本工程里对 `-O0` 最敏感的一类代码，debug 数字会离谱到误导选型。

---

## 6. 尚未验证 / 需 PoC 才能定的

- C 案在真机上的实际 ms（第 3 节表格里那个 2–4ms 是按采样次数推的**估算**，不是实测）。这一项直接决定要不要 P4。
- 1/4 分辨率云边在手机屏 DPI 下用户是否可接受（像素判断归用户）。
- `R16F` 线性过滤在目标机型上是否需要 `OES_texture_half_float_linear`——ES 3.0 core 只保证 R16F 可采样，线性过滤是扩展。若不可用，噪声退 R8 精度够，但云深度重投影会受影响（那是 P4 的事）。
- Blue Marble 云图是**静态一张**（NASA 合成图）。若要"今天的真实云"，就落到真实气象数据图层那条线上，与本文的渲染层通过同一个 coverage 采样接口对接——接口留好即可，本期不实现。
