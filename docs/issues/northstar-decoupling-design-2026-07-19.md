# 北极星 Phase 1：纹理/几何解耦 设计（调研 + 方案 + 待拍板）

> 成文 2026-07-19。对应 [northstar-texture-geometry-decoupling-2026-07-18.md](northstar-texture-geometry-decoupling-2026-07-18.md) 的 **Phase 1**。
> 基线数据见 [northstar-phase0-baseline-2026-07-19.md](northstar-phase0-baseline-2026-07-19.md)。
> **本文不含实现，产出 = 方案 + 待用户拍板的决策清单。**

---

## 0. Phase 0 校准过的问题陈述（比北极星原文更精确）

耦合链未变：高德影像 `isMoreDetailAvailable()` → 地形瓦片 `isRenderable()` 判 false → selector 继续细化 → 把 z12 DEM 上采样到 z13-18 只为托更清影像。

**但 Phase 0 同位姿对拍（elev45，z18 耦合 vs z12 capped）把成本结构测清了**——这改变方案权重：

| 爆的 | 倍数（同位姿） | 不太爆的 |
|---|---|---|
| 地形**渲染瓦片数** | **~22×**（3→65） | 地形几何**字节** 仅 +7~8MB（上采样瓦片是父网格小块 clip） |
| **影像纹理字节** | **~4.5×**（9→41MB） | 常驻**总内存** 仅 1.5×（且被 192MB 缓存预算封顶） |
| selector peak | ~2× | |
| churn（settle 前 notReady/clip 尖刺时长） | 大 | |

**⟹ 解耦要砍的头号目标是"瓦片数量"（连带 draw call / selector 遍历 / 每片 clip 主线程尖刺 / 每片影像纹理 / churn），不是"地形几何内存"。** 这不改北极星方向，但意味着：
- **"cap 地形几何 LOD"本身就拿下大部分收益**（瓦片数 22×→1×、churn 归零、影像纹理回落）——这是低风险的 80%。
- 难点收窄成一句：**capped 的粗地形瓦片，怎么显示比它自己更细的清晰影像**（否则就是 Phase 0 那张糊成一片的 z12 截图，不可接受）。

---

## 1. 参考调研（.ref/ 本地源码）

### 1.1 osgEarth（3.8.1，REX 引擎实现被裁剪，tile-model 机制完整）
- **同款耦合，可 cap**：影像数据可用性默认驱动地形细分；`TerrainOptions::maxLOD` 可硬停地形 LOD（`TerrainOptions:89-96`）。
- **可复用原语 = scale-bias 纹理矩阵**：每 color layer 在 tile 上带 `(texture, osg::Matrixf matrix)`（`TerrainTileModel:28-37`），matrix = 祖先纹理子窗口的 scale-bias，向上爬到有数据的祖先累乘（`TerrainTileModelFactory.cpp:332-354`）。**但它只解"粗影像贴细几何"（向上采祖先）；"细影像贴粗几何"osgEarth 仍要细分几何**——即 osgEarth **无法在比最深几何瓦片更细的 LOD 上画影像**。
- **无虚拟纹理**：megatexture/clipmap/virtualtexture 零命中；`TextureArena` 是 bindless 驻留管理，**按瓦片数有界，非按屏幕分辨率**。
- **⟹ 结论**：osgEarth 证实这是行业默认耦合、给了 cap 手段和 scale-bias 原语，但**不解我们的核心洞**（细影像贴粗几何）。GE 的 Universal Texture 才解，且闭源。成熟开源无现成 C 模板。

### 1.2 cesium-native（我们的移植上游）
_（见 §2，与我们自身机制一并核对——最直接的可复用性判断）_

### 1.3 maplibre
2D/2.5D 栅格渲染，影像-地形关系结构不同，本轮未深挖（需要时另做）。

---

## 2. 我们已有的地基（决定过渡路径的 crux）

> **核心问题：我们（及 cesium-native）是否已有"纹理坐标 scale+translation"，让一个瓦片显示与其几何 LOD 不同 LOD 的影像？** 若有，就是解耦的现成地基。

**答案：有，而且是端到端现成的（1:1 移植自 cesium-native）。** scale-bias 纹理矩阵原语已在树内跑：

| 环节 | 我方 file:line | cesium 上游 |
|---|---|---|
| 计算 scale/translation | `RasterMappedToTilesetTile::computeTranslationAndScale` → `TileSurface::computeTranslationAndScale` (`tiling/TileSurface.cpp:10-30`) | `RasterOverlayUtilities::computeTranslationAndScale`（`rasterUv = geoUv*scale + translation`） |
| 存 + 祖先复用 | `offsetU/V_ scaleU/V_`（`RasterMappedToTilesetTile.h:237-239`），多个几何瓦片共享同一 `_pReadyTile`，走 `ReadyTileSource::Ancestor`（`.cpp:382-419`） | `RasterMappedTo3DTile::_translation/_scale`，ancestor loop |
| 上屏 uniform | `SurfaceRasterBinding` → `cmd.gltfRasterOverlayTileUvs` → `u.mappedRasterTileUv[i]`（`GltfDrawCommandBuilder.cpp:392-419`，`GltfUniformBlock.h:129/256-262`） | 同构 |

**含义（对方案是好消息）**：
- **"一个瓦片显示与其几何 LOD 不同 LOD 的影像"今天就在做**——上采样子瓦片取不到自己 z 的影像时，就复用祖先纹理 + 重算 UV 子窗（`ReadyTileSource::Ancestor`）。**寻址（geometry UV → texture 子窗）这半已完备，不用重建。**
- **但方向是"粗影像贴细几何"**（祖先=更粗影像，子窗采样）。我们要的"**细影像贴粗几何**"用同一寻址原语，缺的是**源**：一个 z12 地形瓦片的绑定纹理里必须**装得下 z18 级细节**——单张纹理装不下 z12 面积的 z18（=64×64 z18 瓦片），且每瓦片最多绑几张 `mappedRasterTileUv[i]`，撑不起一整棵更细四叉树。
- **⟹ 缺的两块精确定位**：
  1. **断细化决策**：`isMoreDetailAvailable` 今天经 `TileRasterUpsampledChildMaterializer.h:42-63`（对齐 cesium `TilesetContentManager.cpp:2140-2149`）**捏造上采样地形子瓦片**来挂更细影像。断这条 = geometry cap（Phase 2a）。
  2. **给 capped 粗瓦片一个"细影像源"**：scale-bias 只负责寻址进这个源；源本身要么 **B 合成**（把覆盖的深 z 影像烘到该瓦片一张纹理）要么 **C 虚拟纹理**（fragment 间接采样共享 atlas）。**这才是真正要新建的部分，且是唯一要新建的部分。**

**cesium 对比**：cesium-native 同款耦合（imagery more-detail 驱动 `createQuadtreeSubdividedChildren` 捏造几何子瓦片），**不独立 cap 几何**；但 raster overlay 有独立 SSE（`RasterOverlay.h` overlay SSE=2.0 vs tileset SSE=16.0），说明"影像 LOD 决策"本就与几何分离，**只是 refine 动作仍耦合**。我们要断的正是这最后一步。

---

## 3. 目标架构与三档评估（用"有界"尺子）

沿用北极星 §4 的三档，按 Phase 0 校准后的成本结构重估：

| 档 | 做法 | 有界性 | Phase 0 校准后裁决 |
|---|---|---|---|
| **A 只拆细化驱动** | 地形按 DEM 误差 cap，影像独立细化 | 地形瓦片有界 ✓ | **不再是"不完整"，而是 80% 主收益**——因为成本头号是瓦片数，cap 掉就赢大半。**但留一个洞**：capped 瓦片的影像怎么显示（见 §2 原语）。**A 是必做的第一步。** |
| **B 逐瓦片合成** | 每 capped 地形瓦片把覆盖它的深影像合成一张纹理 | **取决于纹理定尺**：定死大尺寸(2K)×瓦片 = 爆；**按屏幕覆盖定尺**（tile 占屏面积 × 分辨率）则**屏幕有界** | Phase 0 显示 capped 地形瓦片数很少(3-68)，B 若按屏幕覆盖定尺 → 有界且简单。**可作过渡形态。** |
| **C 共享 atlas 虚拟纹理** | 一张视野 atlas + 间接纹理 + feedback，几何采样虚拟纹理 | 屏幕分辨率封死 ✓✓ | **目标形态**（GE Universal Texture）。移动端固定开销（feedback 回读 stall / 逐 fragment 间接采样）**必须真机 PoC 量**，本 Phase 无法凭空给。 |

**关键再认识（改自北极星）**：B 的"内存随瓦片数爆"批评，前提是"很多瓦片"。**geometry cap 后地形瓦片本就很少**（Phase 0 capped render=3~6），B 若纹理按屏幕覆盖定尺就是有界的。所以 **A（cap）+ B（screen-sized 合成）可能已是"能长成 C 的过渡形态"**，风险和工程量都低于直接奔 C。**C 的增量价值 = 更省的共享驻留 + 更干净的瓦片边界重叠处理 + feedback 精确可见页**，需 PoC 定它值不值那份复杂度。

---

## 4. 几何 cap 精确级别（北极星决策 #2，真实数据算）

FABDEM 源 ~30m。grid65 = 64 interval/tile。重庆纬度 29.617°：

| LOD | tile 宽 | 顶点间距 | 对 30m 源 |
|---|---|---|---|
| z12（当前 native） | 8.51 km | **133 m** | 欠采样 **4.4×**（浪费 3/4 源细节） |
| z13 | 4.25 km | 66 m | 欠采样 2.2× |
| **z14** | 2.13 km | **33 m** | **≈吃满 30m 源** |
| z15 | 1.06 km | 17 m | 超采样（假细分开始） |

**两个选项**：
- **cap = z12（当前）**：最省，但地形几何比 FABDEM 能给的粗 4.4×（山脊/河谷细节没吃满）。
- **cap = z14（需用户重生成 grid65 到 z14）**：地形几何吃满 30m 源，是"真实分辨率"的诚实上限。相对 z12，覆盖区地形瓦片约 16×（但这是**真几何**，非假细分，且仍有界——到 z14 就停，不随影像再涨）。

> 影像通路继续到 z18-19（高德上限），与几何 cap 无关。**几何 cap 级别只决定"地形起伏多细"，不决定"影像多清"**（后者归纹理通路）。建议：**先 cap z12 落地解耦（数据零改），验证收益后再评估是否重生成到 z14**（纯数据侧工作，引擎不变）。

---

## 5. 移动端硬约束核算

- **GLES sampler 上限**：现 glTF shader 在 GLES 已从 20 压到 10 个 sampler（[[gles-gltf-sampler-limit]]，baseColor + raster/water units 压位）。解耦纹理通路若走 C，需额外 sampler（物理 atlas ×1 + 间接纹理 ×1 ≈ +2）；走 B 复用现有 raster sampler 位。**需 Phase 2 核实 GLES 下 sampler 余量**（当前 10 的分配表）。R16F 顶点纹理（若未来上 GPU-VTF 地形）另算。
  - ✅ **已核实（2026-07-19，C-PoC 调研）**：解耦纹理走的是**独立的 terrain 片元 shader**（`kTerrainFragmentGLSL`，非共享的满配 glTF PBR shader），它 GLES 只用 6 个 sampler（unit 0 baseColor + 5-8 raster + 9 water），**unit 1/2/3/4 全空**；Metal `terrainFragment` 只声明 1 个共享 sampler，**1-15 全空**。⟹ **C 的 +2 sampler（物理 atlas + 间接纹理）在 terrain shader 上装得下，两后端都有余量**（满配 PBR shader 零余量的顾虑不适用于地形通路）。
- **纹理压缩**：影像纹理字节是 Phase 0 第二大成本（4.5×）。ETC2/ASTC 压缩是内存有界的关键手段（[[cache-10x-compression-investigation]]），无论 B/C 都该上。**决策 #3 的一部分。**
- **offscreen render pass 已就绪**（`createFramebuffer` 两后端真实现，[[offscreen-render-pass-2026-07-10]]）= B 的逐瓦片合成 / C 的 atlas 烘焙 的基础设施在，不用新建。
- **C 的诚实账（PoC 必量，本 Phase 给不了）**：feedback buffer 回读可能 stall（移动 GPU 回读贵）；逐 fragment 间接采样有固定开销。"有界≠免费"。

---

## 6. 过渡期兼容（北极星决策 #4）

参照上次地形三支柱重构的经验（[[terrain-continuous-lod-redesign-2026-07-17]]）：一次性替换 vs flag 并存。
- 上次一次性替换（QM→heightmap）成功，风险集中但不维护两条路。
- 本次 overlay 系统冲击面最大（`RasterMappedToTilesetTile.cpp` 625 行 + `RasterOverlayTileProvider.cpp` 4660 行，逐真实顶点算 UV、影像 LOD 绑地形 LOD）。
- **建议**：**A（解耦 + cap）可 flag-gated 灰度**（低风险，先验证瓦片数/churn 收益），纹理通路 B/C 再决定替换策略。

---

## 7. 推荐路径（我的技术建议，供拍板）

**分两步，先摘 80%，再决定要不要 C：**

- **Phase 2a — 断耦合 + cap 几何（低风险，80% 收益）**：
  地形按 DEM 几何误差细化，cap 在 native max LOD（先 z12）；影像细化不再驱动地形 refine（改 `isMoreDetailAvailable→refine` 那条链，北极星 §3 / `TileSelectionRasterOverlayPreparer.h:46`）。**立即砍掉 22× 假瓦片 + churn。** 验收 = 同 elev45 位姿，瓦片数/selector/churn 回落到接近 capped-z12 列。
  - ⚠️ 此步 capped 瓦片的**影像**走 §2 已有的 scale-bias 祖先复用 → 只能到"能取到的最细祖先"（≈ z12），即 **Phase 0 那张糊图**。**2a 单独 = 省了资源但近景影像糊**，必须接 2b 才恢复清晰。**寻址原语现成（不用建），2a 只改 refine 决策（断 `TileRasterUpsampledChildMaterializer` 那条捏造）+ 让影像四叉树可独立于地形四叉树继续（影像瓦片无几何/无 clip，很便宜）。**

- **Phase 2b — 有界影像纹理通路（难点，摘剩下的清晰度）**：
  让 capped 粗地形瓦片显示 z18 级清晰影像。**先做最小 PoC 量 C 的移动端固定开销**（feedback 回读 + 间接采样），与 B（screen-sized 逐瓦片合成）对比，**再拍板 B/C**。数据模型从一开始抽象成"页（page）"，撞墙无缝换 C。

**为什么先 2a**：Phase 0 证明头号成本是瓦片数（22×），2a 一步拿下，且风险/工程量远低于纹理通路；2b 的 B/C 抉择需要 PoC 数据，不该在没数前拍。

---

## 8. 决策（用户 2026-07-19 已拍板）

1. **路径 = 分步（已定）**：先 2a 断耦合 + cap 摘 80%，再 PoC 决 B/C。不直奔 C。
2. **几何 cap = 先 z12（已定）**：数据零改，引擎侧改动即可落地验证解耦收益；是否重生成到 z14 后续再评估（纯数据侧，引擎不变）。
3. **影像纹理通路（2b）= 待 PoC 定**：先做最小 C-PoC 量移动端固定开销，对比 B，再拍板。ETC2/ASTC 压缩无论如何都上。
4. **过渡兼容 = 2a flag 灰度**（低风险，推荐路径隐含）。
5. **先补"测量冻结相机" = 是（已定）**：Phase 2 去耦对拍前补，让重载 far 位姿也可复现（冻 `CameraController::update`）。

## 8b. 锁定的 Phase 2 执行顺序
1. **补测量冻结相机**（决策 #5）：`CameraController::update` 加冻结开关 + demo `kMeasureFreezeCamera`，让 far 也可复现。→ 用 Phase 0 `kMeasure*` 同位姿采**去耦前**完整对照（含 far）。
   - ✅ **引擎侧已落地（未提交）**：`CameraController::setMeasurementFreeze(bool)` — 置 true 后 `update()` 完全空转（跳惯性/zoom 惯性/orbit 重建，启用瞬间清零惯性），相机停在初始 `lookAt/viewDistance` 位姿逐帧字节稳定。`SceneCameraConfig::freezeCamera` 贯通，`resetCamera()` 两分支位姿设定后应用。demo `kMeasureFreezeCamera=true` 钉死。单测 `MeasurementFreezeHoldsPoseDespiteInertia`（甩动后冻结→120 帧视图矩阵字节不变）41/41 绿。
   - ⏳ **待用户在真机采样**：开 `kMeasureFreezeCamera` 重建 → 用 Phase 0 各 `kMeasure*` 位姿（含 far-5000）采**去耦前**完整对照列（瓦片数/selector/churn/字节），回填 Phase 0 baseline 文档 far 行。
2. **Phase 2a 断耦合 + cap z12**（flag-gated）：断 `TileRasterUpsampledChildMaterializer.h:42-63` 的 `isMoreDetailAvailable→materializeRasterUpsampledChildren`；地形按 DEM 几何误差细化 cap 在 native max（z12）;影像四叉树独立继续（无几何/无 clip）。验收 = 同位姿瓦片数/selector/churn 回落到接近 capped-z12 列。**预期近景影像此时仍糊**（走已有 scale-bias 祖先复用，只到 ~z12），必接 2b。
   - ✅ **引擎侧已落地（未提交）**：`TilesetOptions::decoupleImageryFromGeometry`（贯通 `EarthSceneConfig`→facade→options）。cut 点 = `TileRasterUpsampledChildCoordinator::createRasterOverlayUpsampledChildren` 早退（`decouple=true` 时不调 `materialize`，不捏造上采样地形子瓦片）；`prefetcher` 的 `createRasterOverlayUpsampledChildren` 决策位不变（保持忠实，只断消费）。几何自然 cap 在 DEM native max（z13+ 唯一来源就是这条捏造）。demo `kMeasureDecoupleImageryFromGeometry`（默认 false=耦合对照）。单测 `decouple gate`（同源 A/B：true→0 子/false→4 子，决策位不变）+ golden 11/11 + child-materializer 54/54 零回归。
   - ✅ **真机 A/B 已采（2026-07-19，debug，冻结相机 elev45/z1500m 同位姿，设备 7e045e39，heightmap 地形 8091）——验收通过**：

     | 指标（settled） | Run A 耦合 | Run B 去耦(2a) | Δ |
     |---|---|---|---|
     | **render 瓦片数** | **68** | **3** | **↓22.7× / -95.6%** |
     | memImageryKB(影像纹理) | 43840 | 10816 | ↓4.05× / -75% |
     | memContentKB(地形几何) | 84261 | 77957 | -7.5%(其次,符 Phase 0) |
     | memTotalKB | 128101 | 88773 | -30.7% |
     | Engine.render.total | 8-14ms | 5-6ms | ~2× |

     **与 Phase 0 基线逐条吻合**(coupled 65 vs capped 3-6;影像 4.5×;几何字节其次)。**2a「断耦合+cap z12 摘 80%」在真机坐实**:头号成本瓦片数 68→3(砍 96%),影像纹理 4×降,总内存 -31%,帧时间~2×。selector/churn 在冻结相机 settled 态被 reuse 门跳过(=0),运动期杠杆需运动场景另测,但静态瓦片数已决定性证明收益。
     - **⚠️ 代价(符预期)**:Run B 的 memImagery 只剩 10.6MB = **近景影像退回 z0-12 走 scale-bias 祖先复用 = Phase 0 那张糊图**。这正是 2a 单独的必然状态,**必接 2b 补清晰度**。
3. **并行 C-PoC**：最小虚拟纹理（一张 atlas + 间接纹理 + 一次 feedback）真机量固定开销，回填 §5 诚实账 → 定 §8 决策 #3（B vs C）。
   - ✅ **骨架已落地（未提交）**：新模块 `renderer/VirtualTexturePage.{h,cpp}`（纯 CPU「页」数据模型：VtPageId + RGBA8 feedback 编解码 + VtPageTable LRU 驻留/间接纹理更新，B/C 共用）+ `renderer/VirtualTexturePoc.{h,cpp}`（GPU 编排：物理 atlas + 间接纹理 + feedback FBO，每帧 feedback pass→**回读(计时)**→解码→页表→写间接纹理）。**新增 `RenderDevice::readFramebufferPixels` 回读 API**（之前完全缺失,§5 头号未知量)——GLES `glReadPixels` + Metal blit→shared buffer+`waitUntilCompleted`（**故意同步 = 量最坏 stall 上界**，两后端已实现,macOS 下 Metal 也编过）。flag 灰度 `EarthSceneConfig::virtualTexturePoc`→facade→`Engine::setVirtualTexturePocEnabled`,每帧 tick,回读 ms 报进 EarthPerf 头行 `vtReadback=`。demo `kMeasureVirtualTexturePoC`（默认 false）。单测 page 9/9 + poc 9/9(mock canned feedback 走通整链)+ 全量 150/150 零回归。
   - ⚠️ **骨架局限（诚实标注）**：feedback pass 只清背景**未接 page-id 片元 shader**,故 on-device 解码可见页恒 0——但**①回读 stall 计时依然有效**（stall 在 GPU→CPU 同步屏障,与画了什么无关)。**②逐 fragment 间接采样固定开销是下一子步**（改 terrain 片元 shader;sampler 余量已核实充足:terrain 是独立小 shader,GLES unit 1-4 空 / Metal sampler 1-15 空,C 的 +2 装得下）。RGBA8 feedback 只到 z14(x/y 各 14 位),z18 需更宽格式(扩 TextureDesc,跟进项)。
   - ✅ **真机首测已采（2026-07-19，debug 构建，feedbackDownscale=8，设备 7e045e39）**：`vtReadback` n=32 → **min 0.76 / 中位 3.84 / 均值 4.81 / p90 9.58 / max 10.35 ms**。**结论：同步 feedback 回读的 stall 在移动端真实且不进帧预算**——单 p90≈9.6ms 就吃掉 60fps 帧预算(16.7ms)大半，max 10.35ms。§5「回读可能 stall」得到实测证实。
     - **测量语义**：`readbackMs` 包 `glReadPixels`(强制冲刷该 FBO 前所有 GPU 命令 + 传 135×300×4≈162KB)。这是 GPU 同步屏障耗时,**GPU-bound、与 -O0 debug 基本无关 → 该数可信**(不同于 [[perf-measured-on-debug-build]] 说的 CPU 侧膨胀)。含「冲刷帧内待完成 GPU 工作」成分,但这正是「往帧中段插同步回读」的真实代价。
     - **⟹ 对 §8 决策 #3 的输入**：**同步回读不可用于生产 C**;C 若上必须**异步化**(PBO 双缓冲 + fence / completion handler,回读延迟 1-2 帧消费),固定开销才可控。**B(逐瓦片合成)完全不需回读通路,天然回避这个 stall**——B 相对 C 的一个实测优势。
     - **⚠️ 骨架读数噪声**:`vtVis` 恒 1 = feedback FBO 被 `beginPass` 清成**天空色**(非背景 0),整帧同色 decode 成 1 假页;`vtUpdate` 15-23ms 是 -O0 下对 ~40K 像素的 decode 循环,**均骨架产物、无意义、忽略**。接 page-id 片元 shader + feedback 清 0 后才有真页数据。
   - ✅ **异步 PBO 回读 + 地平线场景已补（2026-07-19，纠正「同步回读=不公平 C 测试」）**：

     **① 异步回读**（GLES PBO+fence，enqueue 本帧/acquire 隔 2 帧）——near 视野 async：

     | | 同步(旧) | 异步(新) |
     |---|---|---|
     | acquire(map) | — | 中位 **0.10** / p90 0.18 ms |
     | enqueue(readpixels-to-PBO) | — | 中位 **1.1** / max **9.5** ms |
     | 单次回读总 | 中位 3.84 / p90 9.58 ms | 中位 ~1.2 / tail ~9 ms |

     **关键洞见**：PBO fence **只把「传输」延迟成功隐藏了**（acquire ~0.1ms），**但「resolve/flush」躲不掉**——Adreno 是 tiled GPU，`glReadPixels` 读一张刚渲的 FBO 必须先把 tile memory resolve 到系统内存，这笔成本落在 **enqueue** 端，中位降一半但尾部仍 ~9ms。**⟹ C 的 feedback 回读在移动端是个躲不干净的每帧税（中位~1.2ms、尾~9ms），不是我上轮「同步很贵」那么简单，也不是异步就免费。**（可试 feedback FBO 双缓冲让 resolve 隔帧摊，未测。）

     **② 地平线场景**（低仰角 elev10/z12km 宽视野）——**capped 瓦片数是 B/C 的真正分水岭**：

     | 视野 | 耦合 render | 去耦(2a) render | 2a 降幅 |
     |---|---|---|---|
     | 近景 elev45/z1500m | 68 | **3** | 22.7× |
     | 地平线 elev10/z12km | 133 | **122** | **仅 1.09×** |

     **决定性发现**：**地平线视野下 2a 几乎不降瓦片数（133→122）**——宽视野的瓦片数由**基础覆盖**（frustum 要铺满 ~122 个 z12 瓦片）主导，不是影像假细分。所以去耦后地平线仍有 **~122 个 capped 瓦片**需要贴清晰影像。
   - **⟹ 对 §8 决策 #3 的公平结论（修正上一轮的「压向 B」）**：**B/C 不是全局二选一，按视野分裂**：
     - **近景/中景**（capped 瓦片少，实测 3）：**B 完胜**——3 张逐瓦片合成纹理、零回读、复用现成通路。C 的回读税(~1.2ms)+atlas 机器纯属浪费。
     - **地平线/宽视野**（capped 瓦片多，实测 122）：**C 该赢**——1 张共享 atlas vs B 的 122 张逐瓦片烘焙(随相机移动每帧重烘)。这正是调研当初把 C 当终局的理由（屏幕封顶、扛多瓦片），被数据坐实。
     - **诚实缺口**：B 仍未测（122 张逐瓦片烘焙的真实每帧成本没数）。要彻底拍板还需一个 B PoC 量地平线下的烘焙开销，与 C 的回读税对比。
   - ✅ **B PoC 已做（2026-07-19，commit a1b1e56c2，debug，decouple on）——三方数据齐了**：

     | | B 烘焙(re-bake/帧) | C 回读税 |
     |---|---|---|
     | 近景 N=3 | ~0.24ms(pass 地板) | 中位 1.2 / 尾 ~9 ms |
     | 地平线 N=133 pass 切换地板 | **0.38ms** | — |
     | 地平线 N=133 +1 sample/px fill | **中位 1.67 / p90 1.93 ms** | 中位 1.2 / 尾 ~9 ms |
     | 内存 | 33MB(133 瓦片×256²) | 16MB atlas(固定) |
     | settled 可缓存? | **可(不重烘→~0)** | 否(feedback 每帧) |
     | 随内容增长? | fill 随 samples/px 线性(2-4×) | 固定 |

     **决定性发现：推翻「地平线瓦片多 → pass 切换贵 → B 输」的直觉（我和调研都假设错了）**：
     - **pass 切换在 Adreno 可忽略**（133 个 clear-only pass 仅 0.38ms，驱动 fast-path），不是 B 的瓶颈。
     - **B 的 fill 也就 ~1.3ms（1 sample/px），与 C 的回读税（1.2ms）同量级，但 B 的尾（1.93ms）远好于 C（~9ms），且 B 可缓存（settled 不重烘 → ~0），C 的回读每帧都付。**
   - **⟹ §8 决策 #3 再修正（这是第三次校准，每次都被数据纠偏）**：**B 比「瓦片数」暗示的更能打，跨近景/地平线都competitive**：
     - 近景：B 完胜（0.24ms vs C 1.2ms+机器）。
     - 地平线：B 烘焙 1.67ms/尾 1.93 **≈** C 回读 1.2ms/尾 9ms——**同量级、B 尾更好、B 可缓存**。C 的优势收窄到「内存更省(16 vs 33MB)+ fill 不随 samples 涨」。
     - **B 现在是更稳的默认选择**（低风险、复用现成、可缓存、尾部友好），C 留作「samples/px 高 + 相机常动 + 显存紧」三条同时成立时的升级项。数据模型已抽象成「页」故 B→C 可换。
   - ⏳ **剩余诚实缺口**（不阻塞拍 B，是把边界量全）：①B fill 在真实 samples/px（2-4，多覆盖瓦片叠加）重测——线性外推 ~0.38+4×1.3≈5.6ms，仍可缓存;②B 加缓存量摊销成本;③C 接 page-id 片元 shader 量逐 fragment 间接采样与 B fill 对齐;④Metal 异步回读补齐。
4. **Phase 2b 影像纹理源**：按 3 的结论建 B 或 C，让 capped 粗瓦片显 z18 清晰影像（复用现成 scale-bias 寻址原语 §2）。验收 = 同位姿去耦列"斜率≈0"且影像照清。

## 9. 建议的下一个动作（若用户走推荐路径）
- **Phase 2a 第一刀**：断 `isMoreDetailAvailable→refine`，地形 cap z12，flag-gated。验收用 Phase 0 的 `kMeasure*` 同位姿对拍（去耦列应逼近 capped-z12 列，且影像走 §2 原语看能到多清）。
- **并行 C-PoC**：最小虚拟纹理 PoC（一张 atlas + 间接纹理 + 一次 feedback），真机量固定开销，回填 §5 的"诚实账"，再定 §8 决策 #3。

## 10. 多叠加层的影响 + 合成方案（2026-07-19 讨论，推理未量）

### 10.1 多叠加层对 B 的影响：分两类，都不"暴涨"
- **栅格叠加层（K 个：路网 raster / 第二卫星源）**：
  - **内存不暴涨**：B 把 K 层 **flatten 进同一张合成纹理** → 仍 N×256²（33MB），**与层数无关**（B 的一个优势）。
  - **烘焙成本线性涨**：每像素采 K 层 → fill ≈ K×。K=4（引擎 `kMaxGltfRasterOverlays` 上限）+ 地平线 ≈ 5ms。线性非指数。
  - **B 的真软肋 = 动态层**：B 是"烘死"的，某层频繁变（开关/透明度/实时数据）→ 必须**整张重烘** → 缓存优势失效。**这是对 C 有利的、之前偏 B 时没量到的新因素。**
- **矢量数据（路网线 / POI / 可编辑矢量）**：**不该走 B/C**。烘进 256² 瓦片会糊 + 可编辑一改就重烘。应走**独立贴地 pass**（stencil / depth-clamp / 挤出几何，见 [vector-data-system-design-2026-07-07](vector-data-system-design-2026-07-07.md)），成本 = 矢量复杂度、与瓦片数和 B/C 无关。

### 10.2 合成方案：烘焙填充的共享 atlas（B 的烘 + C 的寻址 + CPU 驱动）
**诊断**：B 涨 / C 每帧付，根都是 **per-frame**（B 每帧重烘 N 张，C 每帧回读）。改成 **event-driven** 两者都塌：
- **洞见 1 — C 的回读不必要**：GPU feedback 存在是因为"引擎不知道哪些页可见"。但**我们每帧本就跑 CPU tile selector**，它知道可见瓦片/LOD → **可见页从 CPU 选择器算，无需 GPU feedback、无回读税**。删掉 C 最贵一环。
- **洞见 2 — 烘焙只在"页调入"时做**：页烘进 atlas 后常驻，只烘**新进视野的 frontier 页** → 烘焙成本 = **页调入速率**（受平移速度 + 每帧预算封顶），**不随可见瓦片总数 N 涨**。静止 → 0 调入 → 0 烘。

```
CPU 选择器 → 可见页集合(无 GPU feedback)
  → 新页:烘覆盖影像进 atlas 空槽(B 的烘,只烘新页、进共享 atlas)
  → 片元:采间接纹理 → 采 atlas(C 的寻址)
  → 页常驻,静止不重烘;动态层变只增量重烘受影响页
```

| | 合成方案 | 纯 B | 纯 C |
|---|---|---|---|
| 烘焙成本 | **只烘页调入(不随 N 涨)** | 每帧 N 张 | 0 |
| 回读税 | **无(CPU 驱动)** | 无 | 每帧 ~9ms 尾 |
| 内存 | **共享 atlas(~16MB)** | N×纹理(33MB) | 共享 atlas |
| 动态多层 | **增量重烘受影响页** | 全重烘 | 逐片元多采 |

这正是 **GE Universal Texture / id megatexture 的真实做法**。**⟹ 直接答"烘焙不涨 + 结合 BC 优势"**：烘焙=按页调入的增量事件，寻址=C 的 atlas/indirection，C 最贵的回读被 CPU 选择器替掉。

**地基已在手**：C-PoC 的 `VtPageTable`（页表/LRU/间接更新）= 核心；`TileCompositeBakePoc` 的烘焙原语 = "填目标"。合成方案 = 烘焙目标从"逐瓦片纹理"改成"atlas 槽" + 驱动从"GPU feedback"换成"CPU 选择器可见集"。**不是重起炉灶，是接起两个 PoC + 换驱动源。**

**代价/未量项（这轮教训：别纯推理拍板）**：① 复杂度最高（atlas 槽分配 + 间接增量更新 + 页淘汰 + 烘到 atlas 子区域 glViewport 进槽 + "瓦片→虚拟页"CPU 映射）；② 逐片元间接采样开销未量；③ CPU 从选择器算可见页对我们瓦片方案是否干净未验；④ 动态多层受 GLES terrain sampler 空 unit 数限制。

**更省事退路（Option-lite）**：只烘"静态深层 base 影像"（2a 打破的那块），动态叠加层留在现成逐瓦片 raster draw / 贴地 pass。烘焙只碰 1 静态层 → 无动态多层重烘之苦，复杂度低得多，摘 80% 问题。

**建议**：先起最小原型验证合成方案的两个门（②逐片元间接采样开销 + ③CPU 页 determination 可行性），过门 → 目标形态定合成方案；过不了 → 退 Option-lite。

## 11. 合成方案调研结论（2026-07-19，三路 .ref/ 源码分析）

三个 Explore agent 分别查了：①我方引擎的 CPU 页 determination；②我方 atlas 烘焙/间接机制；③cesium-js `Megatexture.js`+`VoxelTraversal`（体素渲染器，用的正是"CPU 遍历定驻留→打包 atlas→片元间接采样"，机制同构）。

### 11.1 门②（CPU 页 determination）= 基本已解
引擎**每帧已在 CPU 侧算出**每个可见地形瓦片采样哪张影像 `(z,x,y)`（或祖先回落 + scale-bias 窗口），全是 POD 状态，无 GPU feedback：
```
tileset.tilePlan().renderEntries → renderTile → rasterOverlayState.forEachMapping()
  → getReadyTile()->getTileID()      // (z,x,y) = 页 id
  → getScaleU/V, getTranslationU/V   // 页表子窗(已是 scale-bias!)
  → getMappedSourceTiles()           // 复合源列表
```
`RasterMappedToTilesetTile::update` 的祖先回落走 `computeTranslationAndScale`（RasterMappedToTilesetTile.cpp:599-623）。⟹ **C 的 GPU 回读税(9ms 尾)对我们完全不必要，可见页从现成映射白捡。** 唯一细活：跨共享祖先的瓦片去重(cheap hash-set over renderEntries)+ 把 terrain-tile 粒度的映射细化到 sub-tile z18 页粒度(改 imagery SSE 选择 `chooseQuadtreeSourceZoom`，maxZoom=18，不再被 terrain z12 卡)。

### 11.2 cesium Megatexture 的关键启示：**填 atlas 用「上传」不用「渲染」**
cesium 的 Megatexture **不 render-to-texture**，而是把解码后的瓦片数据**上传进 atlas 槽**（`writeDataToTexture`→`texture.copyFrom`=texSubImage，Megatexture.js:423-447）。⟹ **绕开我方最大缺口**（agent②：createFramebuffer 总新建附件、beginPass 硬编码全 FBO viewport、零 scissor、clear 会污染邻槽）——**改用 `updateTextureRegion`（已有）把影像上传进槽**，不需要建"渲进 atlas 子区域"那套后端能力。
- 槽管理 = O(1) 双链空闲表(add/remove)，非 LRU-walk；优先级/淘汰在遍历层(优先队列 capped 到 atlas 容量)。我方 `VtPageTable` 已是页→槽映射，补 atlas 纹理 + 槽上传即可。
- **多层不 bake**：每层一张 atlas（各自上传瓦片进槽），共享同一间接结构；片元逐层采样 blend。**动态层 = 跳过该层采样(切换)/uniform(透明度)，零重烘**——彻底解掉 B 的动态层软肋。内存 = K 张屏幕封顶 atlas。

### 11.3 间接结构 = CPU 建的扁平四叉树纹理
cesium 的"octree texture"= 扁平数组，每节点 = 父指针 + 子槽入口，2-bit flag(INTERNAL/LEAF/PACKED_LEAF_FROM_PARENT)标记(VoxelTraversal.js:933-966)，CPU 每帧 `buildOctree` 深度优先建、整张上传、NEAREST 采样。**移到 2D 四叉树 = 5 texels/节点(1 父 + 4 子)**。片元自顶向下降：每层 1 次 texel fetch(深度受 SSE 上限约束) → 叶 → 槽 index → atlas UV → 采样。**2D 影像贴地比体素便宜**(单次自顶向下降，无 ray-march，无逐步重查)。

### 11.4 落地账（做什么 / 现成 / 可移植）
| 部件 | 状态 |
|---|---|
| CPU 页 determination(可见页+scale-bias) | **现成**(复用 raster 映射, §11.1) |
| 页→槽 CPU 账本(VtPageTable) | **现成** |
| 间接纹理小矩形更新(updateTextureRegion) | **现成**(改 dirty-rect,GLES 要求紧打包) |
| 2D atlas + 槽空闲表 | 可移植(cesium Megatexture 降维,直白) |
| 填槽(上传解码影像进槽) | 需改 raster 瓦片生命周期(留 CPU 像素)或加 GLES blit(Metal blitEncoder 已有) |
| 扁平四叉树间接纹理(CPU 建+上传) | 新建(移植 cesium buildOctree→quadtree,小) |
| 片元:间接降+atlas 采样 | **新建 = 唯一真未知(门①)**;单层 UV-remap 先例在(applyMappedRaster,Renderer.cpp:964-972),sampler 余量够 |

### 11.5 结论 + 建议
- **合成方案比"bake into atlas"初想的更可落地**：cesium 的"上传填 atlas"绕开渲染-into-atlas 缺口；门②(CPU 页)已解；间接纹理可移植;唯一真未知是**门①逐片元间接采样开销**(cesium 证 2D 下 depth-bounded 且便宜，但要在 Adreno 实测)。
- **最小原型**(验门①)：建小 atlas + 扁平四叉树间接纹理 + terrain 片元 shader 加"间接降→采 atlas"，真机量逐片元开销 + 与 B fill(1 sample 1.3ms)对齐比较。CPU 页/页表/上传都largely现成，原型主要是 shader + 间接纹理 builder。
- **⚠️ 缺口**：cesium 的实际 GLSL(`Octree.glsl`/`Megatexture.glsl`)不在 sparse checkout 里，写 shader 前值得从全 clone 取那段寻址数学(降索引→UV、flag 解包、槽→atlas UV)。
- **低风险退路 Option-lite 仍在**：B 已实测可用(1.67ms 可缓存)，静态 base 走 B + 动态层走现成 raster/贴地。合成方案是"更省 + 动态多层友好"的目标形态，不是唯一出路。
