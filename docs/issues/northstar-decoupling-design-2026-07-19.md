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

### 11.6 门① 原型真机结论(2026-07-19,PASS 带设计约束)
`renderer/VtIndirectionSamplePoc.{h,cpp}`(commit c1e66da6c 建台 + 1b41389fe 修正+扫描)。纯旁路测量台,gate 与 camera/scene 无关(合成 fill),`kMeasureVtIndirectionSamplePoC` 一 flag 即测。

**两个测量陷阱(踩坑,重要方法论)**:
1. **纯 CPU 计时量的是「命令编码」非 GPU fill**——tiled Adreno 片元着色推迟到 resolve,`perf::nowMs()` 围 beginPass/submit 只量入队。首测 3.44MP fill 报 0.02ms、descent 反比 baseline 便宜(ratio 0.57)= 荒谬。**修**=每组末尾 1×1 回读强制 flush→resolve→等 GPU(同一 FBO 后画覆盖前画,回读逼所有 pass 执行完),计时纳入真实累计 GPU 时间;再减「仅同步」地板 `runPasses(_,0)` = 干净每-pass GPU fill。
2. **单点会误导**——改一次 run 扫描深度 {1,2,3,4,6,8}(「别外推,直接量」)。

**真机曲线**(7e045e39/Adreno,3.44MP 全屏 fill,已减同步地板,每-pass GPU ms):

| descent 深度 | ms/pass | ×baseline | Δ/层 |
|---|---|---|---|
| base(0,仅 atlas 采样) | ~0.37 | 1.0× | — |
| 1 | ~0.51 | **1.4×** | +0.14 |
| 2 | ~0.70 | **1.9×** | +0.19 |
| 3 | ~1.03 | 2.8× | +0.32 |
| 4 | ~1.37 | 3.7× | +0.34 |
| 6 | ~2.87 | 7.8× | +0.75 |
| 8 | ~9.1 | **24.6×** | +3.9 💥 |

**超线性悬崖**——依赖 fetch 链(下址依赖上次取回值)latency-hiding 失效,深降爆炸。线性外推 D4→D8 会预测 2.7ms、实际 9ms(3.4× 偏差),坐实「别外推」。

**结论 = 门① PASS(带设计约束)**:
- **浅降(1-2 层单次页表,id-Software megatexture 式)+0.14~0.33ms 基本免费 → 合成方案过门,定为 2b 目标形态。**
- **设计约束(硬):间接降必须浅(≤2-3 依赖 fetch),优先「单次 fetch 扁平页表 + mip 回退」,禁 cesium-octree 式深度自顶向下降(≥6 层超线性爆炸)。** 2D 影像本就用不着深降——页表在最细虚拟页分辨率直接存槽指针,一次 fetch 到位。
- 局限:间接寻址是代表性成本模型(N 依赖 fetch + atlas 采样),非 cesium `Octree.glsl` 逐字节等价。写生产 shader 前从全 clone 取寻址数学,但采**单次页表**设计,不照搬 octree 深降。
- **下一步 = 按单次页表设计建合成方案生产原型**:CPU 定页(§11.1 现成)+ `updateTextureRegion` 上传填 atlas(§11.2)+ CPU 建扁平页表间接纹理 + terrain 片元加**单次**间接采样(单层 UV-remap 先例 applyMappedRaster,Renderer.cpp:964-972)。

### 11.7 #3 atlas 工作集真机量(合成方案第三个 gate,已清)
临时插桩 `TileRenderPlanFrameRefresher::refreshFrameProgress`,收集本帧可见地形瓦片实际采样的**唯一影像页 (scheme/z/x/y) 集合**(= atlas 需常驻的页集),报 `AtlasWS uniquePages/renderTiles/mappings/zRange`(**插桩临时,已还原未提交**)。耦合态测量(耦合态影像已是屏幕合适 z,= 合成 atlas 工作集的忠实代理)。真机(7e045e39,冻结相机):

| 视角 | 唯一页数 | zRange | mappings |
|---|---|---|---|
| 近景(elev45/1.5km) | **68** | z16-17 | 68 |
| 地平线(elev10/12.4km) | **~185** | z2-14 | ≈185 |

- **关键发现:`mappings ≈ uniquePages` → 跨瓦片几乎零页共享**(每可见地形瓦片采自己那张影像页)⟹ **atlas 容量必须按峰值可见页数(~185+余量)定,不能指望去重省**。
- **换算(256²×4=256KB/页)**:近 68 页≈17MB,地平线 185 页≈**47MB**。对比现状耦合影像内存 `memImageryKB=77MB` → **atlas 峰值 47MB 比现状还省 ⟹ #3 不是 thrash/内存瓶颈**。设计"~16MB atlas"偏乐观,真实目标 **~48-64MB(256² 页)**。
- ⟹ **三 gate 全过**(门①浅降 + 门②已解 + #3 不 thrash),生产原型无阻塞。

## 12. 架构评审:合成方案 vs 成熟替代(2026-07-19,两路 opus 调研)
基石级复核,对 Cesium/GE/osgEarth/Outerra 做基准核对 + 挑战"是否更优"。

### 12.1 ⚠️ 重大事实纠正:GE Universal Texture = clipmap,不是 page-VT
Chris Tanner(SGI clipmap 1998 原作者)发明 Universal Texture = 把 clipmap 移植到消费级显卡(专利 US7626591B2:逐级独立原点 toroidal clipmap + validity grid 处理部分覆盖)。⟹ **本文档此前"合成方案 = 复刻 GE"的说法从根上不准确**。**合成方案血统实为 id megatexture / SVT,只是 CPU 选择器替掉 GPU feedback ⟹ 正名「CPU 驱动 SVT / 去-feedback megatexture」,非 GE 路线。**(来源:Bar-Zeev "Was Google Earth Stolen?"、US7626591B2、Tanner clipmap 1998)

### 12.2 Cesium 确实解耦(佐证问题真实 + B 覆盖中度解耦)
cesium-native 官方原文:改 tileset SSE **"will not affect the sharpness of the raster overlays"**——raster overlay 有**独立 `maximumScreenSpaceError`**,纹理分辨率=目标屏幕像素/overlaySSE,clamp 到 `maximumTextureSize`。⟹ **"影像/几何 LOD 分离"是行业既有(我们非发明新范式);Cesium 的单纹理 scale-bias = 我们的"B",覆盖中度解耦**。但 **z12 几何/z18 影像极端比值下撞单纹理墙**(z12 覆盖 64×64 个 z18 = 需 16384²/瓦片,或大纹理逐瓦片显存爆炸),且 Cesium 解耦是**静态旋钮**(改 SSE 需整 Tileset 重载,非逐帧)。⟹ **极端比值确需新机器(atlas),这段判断成立。**(来源:cesium.com How Raster Overlays Work)

### 12.3 clipmap 是真备选,且我们此前否定错了
此前假设"clipmap 球面/部分覆盖不可行"——**被 GE 专利本身(球面上跑 clipmap)+ Ellipsoidal Clipmaps(Dünkel/Kang 2015,Outerra 在用)证伪**。clipmap 优势:片元**无间接 fetch**(绕开门①全部风险)、toroidal 增量、球面/部分覆盖有生产先例。**但对我们已被中和**:①门①实测浅降近免费(+0.14~0.33ms),clipmap"无间接"只值 ~0.2ms/片元;②代价=丢弃门②的 CPU 逐瓦片决策复用、另起视点中心 toroidal 平行管线、地平线各向异性同样弱、多层各一栈;③整台引擎建立在 per-tile 四叉树+scale-bias 上,上 clipmap = **架构级重写换已中和的收益**。⟹ **clipmap 存档为长期备选(地平线各向异性/深 LOD 成痛点且愿架构分叉时),非当下路线。**

### 12.4 production 球面引擎实际用什么
| 引擎 | 影像机制 |
|---|---|
| Cesium / osgEarth | per-tile scale-bias(=我们的"B";osgEarth 用 bindless TextureArena 后端) |
| **Google Earth / Outerra** | **clipmap** |
| id / Frostbite | page-VT / SVT(**游戏世界纹理,非行星影像**) |
| **我们的合成方案** | CPU 驱动 SVT(**无现成先例,自研**——文档 §7 已诚实标注,成立) |

**纯 page-VT 在"行星影像贴地"无主流先例**;per-tile 与 clipmap 才是两个生产阵营。合成方案是"per-tile CPU 决策 + SVT atlas 寻址"的杂交。

### 12.5 落地风险表(VT 深挖,补进 §5 诚实账;无 dealbreaker)
| # | 风险 | 会否中招 | 解法 / 落地代价 |
|---|---|---|---|
| **1 页缝/gutter**(**被低估,最该重视**) | **会** | atlas 相邻页纹理空间不相邻,bilinear 跨页取邻页错 texel→接缝。**与 §11.2"上传现成瓦片"简化冲突**:map tile 边到边不带 border,真无缝要每页留 gutter+page-in 取邻页拷边(打破"1 瓦片=1 页独立上传"、放大 #3 调入量)。**退路=片元 per-page clamp**(UV 夹页内)保底到**现状半 texel 缝质量**,不改善但独立上传保住。**原型期先做 clamp vs gutter 观感对拍。** |
| **2 手动 mip** | **基本不中招(架构红利)** | CPU 逐瓦片选页→LOD 由选页定、硬件单页内正常 mip,**不用写 `textureGrad`**。经典单体 SVT 头号麻烦我们结构上没有。 |
| **3 地平线掠射各向异性** | **会(打在英雄场景)** | 单页跨大深度范围无 aniso→走样;id-Tech5 实证 AF 限~4x。VT **和 clipmap 都弱**。每页 mip 缓 minification;真 aniso 移动端太贵一般放弃。**残留观感风险非阻塞——明确接受地平线无桌面级锐度。** |
| **4 thrash/page-in pop** | **会(仅运动瞬态)** | #3 证的是**冻结稳态容量**不 thrash,**未证运动态**;快平移页需求无硬上限。上传预算封顶 + **粗祖先页常驻(缺细页显示糊而非空洞)**。呼应 [[ge-loading-experience-gap]]。 |
| **5 球面寻址** | **不中招(架构红利)** | 页表复用现成 web-mercator (z,x,y) scheme,零新增;极区/反经线由现有 raster 映射处理。 |
| **6 texSubImage 灌 live atlas ghosting** | **可能中招(移动端 gotcha,未提)** | 向本帧正被采样的大 atlas texSubImage,部分驱动 rename 整张(47MB)或 stall。**需真机验 Adreno**;解=双缓冲/staging blit/PBO/确保上传区本帧不采样。已有 `updateTextureRegion`+Metal blit 基础。 |
| **7 页表编码精度** | **边界** | RGBA8 编槽索引:16×16=256 槽/轴 R/G 各 8bit 刚够,**再塞 level/scale-bias 就超精度→错页**。**用 RG16/RGBA16,别用 RGBA8。** |

### 12.6 结论
**合成方案不是抽象全局最优,但在"我们这台 cesium-native 移植引擎"的具体约束下是正确工程选择**——理由是架构复用最大化(门②/scale-bias/updateTextureRegion 现成)+ 三 gate 已过 + 风险已中和,而非碾压 clipmap。**没有任何方案在我们的架构约束下严格优于它。** 拍板附带:①文档正名「CPU 驱动 SVT」;②补 §12.5 三条新风险(页缝/各向异性/上传 ghosting)+ 页表用 RG16;③**原型第一刀先解页缝**——先钉最尖锐的新未知(**⚠️ 已被 §13 取代:改用 texture2DArray 存储直接规避页缝,Step 1 层数上限已过**);④clipmap 存档长期备选。

来源:cesium How Raster Overlays Work、US7626591B2、Bar-Zeev、Tanner/Migdal/Jones 1998、Ellipsoidal Clipmaps(Dünkel/Kang 2015)、Sean Barrett SVT(GDC2008)、van Waveren Software Virtual Textures 2012、Sagristà Sparse Virtual Textures 2023、Dammertz VT notes、fgiesen Android texture uploads、id-Tech5 megatexture 过滤分析。

## 13. 存储后端定案:texture2DArray(取代 §12.6③ 的 clamp-vs-gutter 计划)
评审后追问"范式内更优解"→ 范式已穷尽(per-tile / clipmap / paged 三选一),但**物理存储层有更优变体**:合成方案的页不放进一张 2D 大 atlas,改放进 **`texture2DArray`(每页 = 一个 layer)**。**这直接规避 §12.5 风险 #1(页缝),取代 §12.6③ 的"先做 clamp vs gutter 对拍"计划。**

### 13.1 为什么 array 消灭页缝
"页缝"拆两个子问题:
- **(A) 错内容渗色**(2D atlas 独有,严重):相邻页物理挨着塞,bilinear 在页边缘采样跨进**无关瓦片** → 渗色 + 随相机移动闪。**这是 2D atlas 引入、现状没有的新问题。**
- **(B) 半 texel 边界不连续**(所有分块方案都有,轻):真正相邻两瓦片公共边,各自 clamp 不混合 → 半 texel 硬跳变。**= 现状逐瓦片 clamp 早已接受的行为。**

`sampler2DArray`:第三坐标 layer **层间不插值**(硬件规定)+ 每层 `CLAMP_TO_EDGE` → **物理上没有隔壁页可渗 → (A) 从根上不存在**,不需 border/gutter/per-page clamp,§11.2"上传现成瓦片、1 瓦片=1 页独立上传"简化完整保住;(B) 降回 = 现状质量。⟹ **array 复现现状已接受的视觉,页缝新风险从"要对拍"变成"不用处理"。**

### 13.2 array 对流水线的影响(基本更简单)
- **片元**:仍**单次**间接 fetch(页表给 layer)→ 采 `sampler2DArray`(门① 结论原样适用,不引入新依赖链)。
- **页表**:存 layer 索引(≤2048=11bit,顺带缓解 §12.5 #7 精度)。
- **上传**:`updateTextureRegion` 加 layer 维(GLES `texSubImage3D` / Metal slice,**标准能力,非我们的后端缺口**)。
- **mip / 内存**:每层独立 mip(缓 §12.5 #3);内存与"2D atlas 且 1 瓦片 1 页"同(47MB)。
- **旁证**:≈ osgEarth bindless TextureArena 的 array 版 → 更贴成熟球面引擎做法。

### 13.3 Step 1 真机结果(层数上限 = array 唯一可能否决点,已过)
commit `1a0b5ffde`,`RenderDeviceGLES::onSurfaceCreated` 一次性探测:**Adreno 730 `GL_MAX_ARRAY_TEXTURE_LAYERS=2048`**(vs 地平线工作集峰值 185 页 = **11× 余量**,叠 4 层 740 也稳)、`GL_MAX_3D_TEXTURE_SIZE=2048`、`GL_MAX_TEXTURE_SIZE=16384`;**Metal 规格保证 2048**。⟹ **array 不受层数限,存储后端定案 texture2DArray。**

### 13.4 剩余原型步骤(Step 2/3,开始动生产渲染)
- **Step 2 ✅ 代码已落地**:`TextureDesc.arrayLayers` + `RenderDevice::updateTextureRegion` 加 `layer` 维,两后端各建 `texture2DArray`(GLES `glTexImage3D`+`glTexSubImage3D`、Metal `MTLTextureType2DArray`+`replaceRegion:slice:`)并可上传一页进指定 layer;越界 layer 返 false;每层 `CLAMP_TO_EDGE`(§13.1 消灭页缝)。主机 152/152 绿 + Android arm64 NDK 编译 clean。
  - **ghost 验证并入 Step 3**:ghost(§12.5 #6)只有在 array **正被采样**时驱动才会对 texSubImage rename/stall;Step 2 单独存在时 array 未接进任何 draw、无 live-sampling → 风险无从激发。故 texSubImage 灌 live array 的 ghost 真机观测天然随 Step 3(array 上屏被采 + 拖动中持续灌层)一起量,不单独造不采样的假 probe。
- **Step 3**:terrain 片元从 `sampler2DArray` 按页表 layer 采样,一个 capped 粗瓦片显示正确高清影像,验①路径通 ②边界 = 现状;**并顺带量 Step 2 的 ghost**(拖动中对 live array 持续 texSubImage,看是否 rename/stall)。拆两刀:
  - **Step 3a ✅ 代码已落地(commit `e49ba2fe7` 渲染路径管线 + `ed5de3871` TerrainPageStore)**:整条渲染路径 + array 绑定先用**合成图案**隔离验证(把真实影像 fetch 的数据通路风险隔到 3b)。
    - 管线:`GltfUniformBlock.pageStoreParams`(enabled/gridN/layerBase)过三方镜像;RenderCommand 加 array 纹理槽 20;两 terrain 片元 gated 采 `sampler2DArray`(mesh UV 落 gridN×gridN 格 → layer → 单次采样 alpha-over);Metal 绑 slot20 走共享 terrain sampler、GLES 按 `GLTexture::target()` 走 `GL_TEXTURE_2D_ARRAY` 分支 + `u_pageStore`@unit10(≤16 底线)。**enabled=0 全惰性 → 非目标瓦片逐字节走现状,零回归。**
    - 页存储:新 `renderer/TerrainPageStore`,建 `texture2DArray` 逐层灌红/绿/蓝/黄;经 `Renderer` 裸指针挂进 `GltfDrawCommandBuilder`,`applyPerFrameCommandState` 锁定绘制序**第一个真实地形瓦片**为目标,挂 array + 置 enabled=1。flag `terrainPageStore`(demo `kEnableTerrainPageStore` 默认关)。host 152/152 + Android arm64 core 链接 clean。
    - **真机验收 ✅ PASS(Adreno 7e045e39,commit `bd47e7709`)**:冻结相机全挂诊断下**整屏地形显规则 2×2 红绿蓝黄象限格**(逐瓦片 UV[0,1] 干净、象限间无渗色=CLAMP_TO_EDGE 生效、正确贴合 3D 地形)= terrain 片元采 `sampler2DArray` / array 创建上传 / 格→层选择 / uniform 整链点亮,`glError=0`。**修真机暴露的 GLES bug**:`RenderDeviceGLES::submit` 的 `currentTextures` 定容 = `kGltfWaterMaskTextureSlot+1`=20,既是逐 unit 绑定缓存也隐式界定纹理绑定循环最大 vec 索引 → 把页存储 slot20 排除出循环 → array 永不绑定(单瓦片无色根因);改 +1 到 `kGltfPageStoreArrayTextureSlot+1`(Metal 侧 `maxMaterialTextures` 此前已同类修,GLES 孪生漏网)。目标瓦片选择改**屏幕空间误差最大**(nadir/最占屏,3b 真实影像正需高清处)替"绘制序第一个"。**ghost 未在 3a 激发**(合成图案一次性填充、无 live 上传),随 3b 一起量。
  - **Step 3b(待做)**:页填充换真实更深 raster 影像(`ImageryProvider::requestTile` 拉目标瓦片的更深子瓦片 → `updateTextureRegion(layer)`;UV 用 `TileSurface::computeTranslationAndScale`),gridN 对齐真实子瓦片布局 → 目标瓦片显真实高清、比邻居更锐(①);拖动中 live 上传各层顺带量 ghost(§12.5#6)。
- 任一步卡住 → 退 2D atlas + per-page clamp(§12.6③ 原计划,无损)。

### 13.5 测量台清理清单(合成方案生产原型落定后一并删)
门①/②/③ 与 B/C 决策已用完的**旁路测量台**,生产原型合入后应清理(它们是决策期脚手架,非生产代码):
- `renderer/VirtualTexturePoc.{h,cpp}`(C 回读税测量,B/C 决策已定)+ `renderer/VirtualTexturePage.{h,cpp}`(页模型,若生产页表另起则删,否则提升为生产)+ 单测。
- `renderer/TileCompositeBakePoc.{h,cpp}`(B 烘焙测量)+ 单测。
- `renderer/VtIndirectionSamplePoc.{h,cpp}`(门① 间接采样测量)+ 单测。
- demo `kMeasure*PoC` flag(`MinimalGlobeDemoConfig.h`)+ `EarthSceneConfig` 的 `virtualTexturePoc/tileCompositeBakePoc/vtIndirectionSamplePoc` + facade/Engine 接线 + EarthPerf 头行 `vt*/b*/vti*` 段。
- 保留:`RenderDevice` 的 `readFramebufferPixels/enqueue/acquireFramebufferReadback`(通用回读能力,可能生产复用)、`onSurfaceCreated` 的 array-layer cap 探测(一次性能力日志)、测量冻结相机(`kMeasureFreezeCamera`,调试有用)。
