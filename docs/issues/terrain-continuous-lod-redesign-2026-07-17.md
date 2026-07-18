# 地形连续 LOD 重设计（GE 级）——三支柱方案

2026-07-17。目标：根治 geomorph「从地壳浮上来」，达到 Google Earth 级的连续 LOD 观感。
基于三路源码调研（当前架构 / cesium 基准 / osgEarth 技术）+ `.ref/` 本地分析，非凭记忆。

---

## 0. 问题定性（为什么要重设计，不是打补丁）

当前 geomorph（变体 A，已提交 `ac5fd45a2`）在低-中缩放会「从地壳内部浮上来」。真机 GEODIAG3 插桩坐实根因：

- morph 起点采样的「父级」是**最近可渲染祖先**（`TileRenderPlanFinalizer.h:289-303` 的 `findNearestRenderableAncestor` / `TileGeomorphHeightDelta.cpp:259-279` 的 `findParentSurface` 各自爬多级），**不是严格 z-1**。
- 实测：childZ=7 采到 parentZ=5、childZ=6 采到 parentZ=5，heightDelta 达 **-2230m ~ +7840m**。粗祖先把山峰抹平 → morph 起点落在真实地表下方数千米 → 浮。
- 本质权衡：大 LOD 跳变要么「浮」（morph 走完那几千米）要么「pop」（瞬时跳），**无温和中间态**。补丁式「只在温和处 morph」= 技术债，不根治。

**根治只能换地基**（用户裁决 2026-07-17）：不断层金字塔 + 规则栅格 + 距离连续 morph。

---

## 0.5 决策锁定（2026-07-17 用户拍板）

- **N = 65**（2⁶+1）。起步值；两侧各一个参数，真机可调（draw 卡→129，加载钝→33）。用户 QM 管线已用 65 → 数据侧 N 零改动。
- **渲染模型 = GPU 高度纹理 + 全局共享平面格网**。每瓦片只上传 65×65 高度纹理；vertex shader 采样高度沿椭球法线位移。**不建 per-tile 网格/VBO** → 加载最便宜（单瓦片上传 ~8.5KB 纹理 vs ~100KB VBO）。共享格网 → overlay UV 天然均匀。
- **数据输出 = raw uint16 高度网格**（每瓦片 65×65）。用户改 rasterio 输出器（采样/金字塔/native 复用，只换写入器）。
- **morph 机制（GPU 纹理下自然落定，消解嵌套/父采样两难）**：morph 目标在 shader 里对**本瓦片高度纹理**双采样——fine=最近纹素，coarse=2× 间距双线性（= osgEarth 邻居平均，bilinear 自动给出）；`morphedH = mix(coarseH, fineH, t)`。**全本地、无需父纹理、无需严格嵌套金字塔**（coarse 是 self 下采样，自洽）；跨层接缝因父子同一 DEM 近似无缝。→ **路 X（抽取金字塔）/路 Y（采 z-1）之争在此消解，两者都不需要**。数据源不必嵌套对齐（用户现有逐层独立生成即可）。
- **距离连续 t**：每瓦片按 SSE/距离算 morphStart/End，shader `t = clamp((d-start)/(end-start), 0, 1)`。
- **overlay**：P1 先跳过（纯地形或临时单色），共享格网均匀 UV 迁移留 P4。
- **装机**：测试区（重庆）瓦片打进 **APK assets**（免服务器/adb reverse，与用户「弃本地 8090」偏好一致；ion 用不了因只发 QM）。

## 0.6 P0 规格（数据侧 + 引擎侧并行）

**数据侧（用户）**：
- rasterio 65-grid 采样器换输出：不写 QM `.terrain`，写 **raw uint16 65×65 高度网格/瓦片**（含 1px halo 边=67×67 存储，供算法线/无缝边，用户 Terrain-RGB 已有此模式）。
- 编码（提议，可调）：`height_m = raw_u16 × 0.2 − 1000`（范围 −1000..+12107m，0.2m 步，够视觉）。nodata：重庆无洋先忽略/当海平面，后续加 sentinel 或掩膜。
- 瓦片方案：**web-mercator**（用户拍板）——对齐高德 overlay，P4 省整套地形/影像重投影机器。rasterio 从 geodetic 改 web-merc 切片（raster-dem 那条本就是 web-merc，可借）。代价：到不了极区（±85° 截断，本项目可接受）。
- 先出重庆小范围 z0-12。

**引擎侧（我）**：
- `HeightmapTerrainProvider` 加 raw-uint16 解码 → `DecodedHeightmap{tileSize=65, heights}`（现有死代码解 Terrarium/Mapbox PNG，加个 raw 格式分支）。
- 加载 APK assets 里的测试瓦片，解码，**抽样高度对 FABDEM 已知点**。
- **P0 达成信号**：引擎解出正确高度栅格（抽样误差 < 编码步长）。

**范围（决定引擎侧怎么建，待拍板）**：新地形路径与旧 QM **并存 + flag 切换**（推荐，改造全程零回归、可 A/B）vs 一次性替换。

---

## 1. 三路调研结论（基准与技术）

### 1a. Cesium（我们的血统基准）——确认不做地形 morph
- cesium-js + cesium-native 全仓 `geomorph` 零匹配；`GlobeVS.glsl` 里 "morph" 只是 3D↔2D 场景变形，与 LOD 无关。地形 LOD 是**帧内硬切 + kick**（`QuadtreePrimitive.js:732-826,904-963`）。
- 防洞靠 **skirt 下沉遮挡**（不真缝合）+ **TerrainFillMesh**（仅缺数据时，`TerrainFillMesh.js`；大瓦片退化为 9×9 规则栅格常高度）。**cesium-native 无 TerrainFillMesh 等价物**。
- 层级 gapless 物化（遍历逐层），渲染可在任意满足 SSE 层停；父级存活靠 LRU 非硬常驻。
- cesium-native 有 `lodTransitionFadePercentage`（alpha 淡入，默认关）——但**是否对地形生效未证实**（可能算了没用）。
- **结论：GE 连续 morph = 明确偏离 cesium 全部四个维度**（顶点渐变/边界/LOD切换/父级存活）。

### 1b. osgEarth（要迁移的技术）——邻居平均 morph（关键）
- 规则栅格 `tileSize = 2ⁿ+1`（默认 17，`TerrainOptions:20`）。
- **morph 目标 = 瓦片内邻居平均，不碰父瓦片对象**（`TileMesher.cpp:84-90` `getMorphNeighborIndexOffset`）：奇数行/列顶点的 morph 终点 = 相邻偶数顶点。数学上偶数格点 = 粗一级 LOD 格点（父格距 2×），所以奇数点的父级位置 = 左右偶数邻居中点。**自包含、O(1)、无需父级常驻、不会浮。**
- **距离连续 morph 因子**：每瓦片暴露 `MorphStartRange/MorphEndRange`（`PatchLayer:126-130`），shader 里 `t = clamp((camDist - start)/(end - start), 0, 1)`，`finalPos = mix(selfPos, neighborPos, t)`。`LODMethod::CAMERA_DISTANCE`（也支持 SSE，`TerrainOptions:40`）。
- 约束顶点（道路/水面）neighbor 设为自身 → 不 morph（`TileMesher.cpp:483-484`）。skirt 顶点继承邻居一起 morph。
- **迁移铁律（osgEarth 自己点破）**：邻居平均 = 父级真实面，**当且仅当父子高度来自同一连续 DEM**（嵌套对齐）。否则平均值 ≠ 父级采样 → 伪影。**TIN 上这招不成立**（无廉价邻居平均）——这正是我们必须换规则栅格的根本原因。

### 1c. 我们当前架构（要改的现状）
- 地形 = QM TIN（`QuantizedMeshParser.cpp`），渲染路径**无任何规则栅格**。
- **已有死代码可复用**：`HeightmapTerrainProvider.{h,cpp}` + `DecodedHeightmap{tileSize,heights}`（`TerrainProvider.h:27-48`）= Terrarium/Mapbox-RGB 高程图解码器，**写了没接线**（`project-health-audit-2026-07-17.md:73` 确认）。
- **已有可复用件**：`EllipsoidTerrainMeshBuilder` 生成规则栅格网格（fill 代理用）；geomorph 顶点管线全套（`TerrainGpuVertex.heightDelta@28`、`geomorphUpFactor` uniform、shader morph 公式 `Renderer.cpp:899-900/2043`、CSR 采样）。
- morphFactor = `lodTransitionFadePercentage`，**纯定时**（`TileLodTransitionController.h:62-64`），非距离——是③要换的地方。
- hierarchy 物化 gapless（`TileContentAccess.cpp:115-183` 总 z+1），但渲染/geomorph 用「最近可渲染祖先」爬多级——是「浮」的结构来源。
- 疑似死字段：`positionHighEcef/positionLowEcef` 写而不读（待确认）。

---

## 2. 数据源规格（地基，用户可自行生成）

用户可生成目标源 → 直接产出下述格式，绕过 ion QM / Terrarium 各自妥协。

**规则栅格高程图金字塔，三条硬约束：**
1. **规则栅格**：每瓦片 N×N 高度，**N = 2ᵏ+1**（推荐 33 或 65）。
2. **嵌套对齐**：子瓦片偶数下标点高度 == 父瓦片对应点高度（逐点相等）。做法 = 同一母 DEM，每级分辨率翻倍、采样格点严格包含上一级格点。**这是无缝 morph 的数学前提。**
3. **不断层金字塔**：root→maxZoom 每级都有（可重采样自母 DEM）。

- 仅需高度；法线从高度梯度算（加载或 shader），不用烘焙。水掩膜可选。
- 磁盘格式随意（PNG/Terrarium RGB 编码 / R16 原始）；引擎侧 `DecodedHeightmap` 解成 float 栅格。
- 瓦片方案沿用现有（geographic/web-mercator TMS）。

**风险**：嵌套对齐若没做严格（父子采样格点不重合），邻居平均 morph 会在瓦片边界产生接缝/伪影——需在生成端保证，或引擎侧加边界特殊处理（退路）。

---

## 3. 三支柱在我们引擎的落法

### ① 不断层金字塔（gapless）
- 物化已 gapless（总 z+1）。要新增的是**渲染不变量**：任意时刻渲染集合里相邻瓦片 z 差 ≤ 1（morph 只处理 ±1 边界）。
- 统一「爬祖先」语义：现在 `TileRenderPlanFinalizer` 和 `TileGeomorphHeightDelta` 各爬各的。连续 LOD 下 morph 不再需要爬祖先（目标是本瓦片邻居），但**渲染 fallback**（子瓦片没加载好时顶替）仍需——需重新设计二者关系，且保证过渡期父级几何存活（不被 LRU 提前淘汰）。

### ② 规则栅格（新地形路径）
- 新建：高程图瓦片 provider（接线现有 `HeightmapTerrainProvider`）→ 解码 float 栅格 → **规则栅格网格生成器**（扩展 `EllipsoidTerrainMeshBuilder`：椭球面规则格网 + 按高度沿法线位移）。
- 顶点携带 **morph-neighbor**（osgEarth `vert_neighbors` 式，本瓦片内奇偶邻居）——扩展 `TerrainGpuVertex`：现有 `heightDelta@28`（标量沿 up）可复用，但邻居平均更通用的是存**邻居顶点位置差**（Vec3）或**邻居高度**（标量，规则栅格下够用，因为水平位置由邻居索引隐含）。倾向：存邻居高度差（标量，复用 `heightDelta` 通道语义微调）。
- overlay UV：规则栅格 UV 均匀 → 大幅简化现有「逐真实 TIN 顶点算 UV」的耦合（`RasterMappedToTilesetTile` 那套）。**这是白捡的简化，但也是最大的改动面**（overlay 投入很大，见 §5）。
- QM 路径退役：`QuantizedMeshParser`/QM content loader/skirt 元数据等一大块 cesium-native 移植退役或保留为可选源。

### ③ 距离连续 morph
- 顶点管线全复用（shader morph 公式、geomorphUpFactor 通道），只换两处：
  - **morph 目标**：从「父采样 heightDelta」→「本瓦片邻居平均」（shader 里由邻居数据算，或 CPU 预填邻居差）。
  - **进度来源**：从 `TileLodTransitionController` 定时 → **距离/SSE 连续函数**。每瓦片算 morphStart/morphEnd（从 SSE 阈值或瓦片几何误差×距离因子反推），shader 每帧 `t = clamp((d-start)/(end-start),0,1)`。
- morph 因子进 uniform（复用 `geomorphUpFactor.w` 通道，但改为每帧按距离算而非定时器盖章）。

---

## 4. 可复用清单（不是从零）

| 组件 | 现状 file:line | 复用方式 |
|---|---|---|
| `HeightmapTerrainProvider` + `DecodedHeightmap` | 死代码，`TerrainProvider.h:27-48` | 接线为生产地形源 |
| `EllipsoidTerrainMeshBuilder` | 生成规则栅格（fill 用） | 扩展为高度位移地形网格生成器 |
| `TerrainGpuVertex` + heightDelta 通道 | `GltfRenderGeometryBuilder.h:41-73` | morph 数据载体，微调语义 |
| shader morph 公式（GLSL/MSL） | `Renderer.cpp:899-900/2043` | 复用，改 morph 目标来源 |
| `geomorphUpFactor` uniform | `GltfUniformBlock.h:44-49` | 复用，w 改距离驱动 |
| 距离/SSE 计算 | selector 已有 SSE | 复用算 morphStart/End |
| skirt 逻辑 | QM parser 内 | 规则栅格版重写（osgEarth `addSkirtDataForIndex` 式，邻居继承） |

---

## 5. 潜在问题（精度/性能/内存/扩展性，诚实全列）

- **精度**：规则栅格均匀密度 vs TIN 自适应——同观感下顶点更多，或尖锐山脊/峡谷被均匀采样抹掉。N 越大越准但越贵。嵌套对齐没做严格 → 边界接缝。
- **性能**：规则栅格顶点数 > TIN（TIN 高效放点）。但 GPU morph 便宜、draw 数可控（每瓦片仍 1 draw）。net 需实测。geomorph 主线程采样（当前 1.6-2.6ms/瓦片）**在新方案里消失**——邻居平均是本瓦片内 O(1)、可在网格生成时一次填好，不再有跨瓦片父采样。
- **内存**：规则栅格高度栅格 + 每顶点 morph-neighbor 数据。N=65 → 4225 顶点/瓦片。
- **扩展性/冲击面（最大）**：**overlay raster 系统**（投入极大，逐真实顶点算 UV、`RasterMappedToTilesetTile`）与 TIN 网格深度耦合。换规则栅格后 UV 变均匀 = 简化，但**这层要重做**。矢量数据贴地（[[vector-data-system-design-2026-07-07]]）、拾取、相机贴地采样（`LoadedTerrainHeightSampler`）都依赖当前地形网格表示，需一并迁移。
- **偏离 cesium**：整条地形数据/渲染路径偏离 cesium-native 移植——AI_INDEX / 既有测试 / golden 需大改。这是架构级重构，不是手术式修改。

---

## 6. 分阶段计划（每阶段可验证）

> 强制目标形式：每阶段给二元「达成 vs 未达成」信号。

**P0 数据源就绪**：用户生成/提供一个小范围（如重庆）嵌套对齐规则栅格高程金字塔（N=33/65，几级）。验证：引擎 `HeightmapTerrainProvider` 解码出正确 float 栅格，抽样高度对得上母 DEM。

**P1 规则栅格地形渲染（无 morph）**：接线高程图源 → 规则栅格网格生成 → 上屏。morph 关。验证：真机出图，地形起伏正确，glError=0，native 编译+新契约测试绿。**先不碰 overlay**（先纯地形或临时单色）。

**P2 邻居平均 morph（距离连续）**：填 morph-neighbor + shader mix + 距离驱动 morphFactor。验证：真机拖动/缩放**无「浮上来」**（GEODIAG 记 morph 目标 = 邻居、delta 有界）；单级过渡平滑无 pop。

**P3 gapless 渲染不变量 + 父级存活**：保证相邻渲染瓦片 z 差 ≤1、过渡期父级几何不被淘汰。验证：快速缩放无跳级 pop、无洞。

**P4 overlay 迁移**：raster overlay 接规则栅格均匀 UV。验证：高德/影像正确贴合，无错位。

**P5 收尾**：skirt/边界接缝、拾取/贴地采样/矢量迁移、QM 路径退役或保留可选、AI_INDEX+测试+golden 更新。

---

## 7. 待用户拍板的决策

1. **数据源**：用户自生成嵌套对齐规则栅格金字塔（首选，最干净）——已倾向，待确认 N 值（33/65）与瓦片方案。
2. **morph 数据载体**：邻居高度差（标量，复用 heightDelta 通道）vs 邻居顶点位置（Vec3，更通用）。倾向标量。
3. **地形渲染模型**：CPU 烘焙高度进顶点（贴合现有 TerrainGpuVertex 管线，推荐）vs GPU heightmap 纹理位移（osgEarth 新版方向，更 GPU-driven 但改动更大）。倾向 CPU 烘焙。
4. **QM 路径**：退役 vs 保留为可选源（多源支持）。
5. **overlay 迁移时机**：P1 先跳过 overlay 验证纯地形，还是一并做。倾向先跳过。
6. **范围**：一次性全量重构 vs 新地形路径与旧 QM 并存、flag 切换、逐步迁移。

---

## 相关记忆
[[ge-loading-experience-gap-2026-07-17]]（浮上来根因 + geomorph 变体 A 历史）
[[overlay-terrain-coupling-filmesh-2026-07-08]]（overlay 硬依赖 TIN 网格，本次要解耦）
[[vector-data-system-design-2026-07-07]]（矢量贴地依赖地形表示，需一并迁移）
[[cache-10x-compression-investigation-2026-07-10]]（顶点内存，规则栅格影响计量）
