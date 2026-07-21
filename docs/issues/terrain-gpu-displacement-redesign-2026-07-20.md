# 地形 GPU 位移重构（GE / osgEarth-REX 模型）——深入分析

> **北极星 Phase 2c（几何侧节点，2026-07-20 纳入）**。见 [northstar-texture-geometry-decoupling-2026-07-18.md](northstar-texture-geometry-decoupling-2026-07-18.md) §7c。
> 定位：北极星「有界资源」目标函数在**地形几何轴**的实现——per-tile CPU 网格/VBO（135KB）→ 共享模板 + per-tile 高度纹理（8.5KB）+ shader 位移，令地形 VBO 字节有界（§5 闸门），并消灭斜视地平线卡顿。与纹理侧 Phase 2b（Universal Texture）正交、可并行、架构收敛（两侧统一到共享几何+per-tile 纹理+shader 采样）。

2026-07-20。承接 `terrain-continuous-lod-redesign-2026-07-17.md`，用**当前 b20 架构实测**+**osgEarth REX 生产引擎源码调研**（`.ref/osgearth`、`.ref/cesiumjs`）更新。
起因：斜视地平线卡顿根因坐实 = 地形 fill-proxy **逐瓦片 CPU 建 289 顶点网格 + per-tile VBO**（掠视运动期 ~29 瓦片/帧首建，prefetchFill 8-29ms + selTrav 12ms，release ~20-30fps）。用户裁决：不打增量补丁，直接投入 GPU 位移 Clipmap 重构（同时根治 fill 卡顿 + geomorph 连续性）。

> **⚠️ 2026-07-20 复审更新（战术修已并入）**：`feat/offscreen-render-pass` 的 `b3b717395`（fill-proxy rebuild 风暴战术修：`hasProtectedRetryableFill` 保护状态集加 `Failed` 终态）已 merge 进本分支（merge commit `a97b3f5eb`）。**该修已把 prefetchFill 40ms→~1ms、整帧 45-53ms→~11-18ms**（release 真机，见 [[fill-proxy-rebuild-storm-fixed-2026-07-19]]）。⟹ **本重构的「消急性卡顿」理由已基本被战术修兑现**，Phase 2c 的价值应重锚到 **§5 闸门「地形 VBO 字节 斜率≈0」（有界几何）+ geomorph 连续性 + 架构收敛（与 2b 统一到共享几何+per-tile 纹理+shader 采样）**，**不再是救火**。残留掠视成本 = 周期性 full-selection 帧 selector ~11ms（是**另一条杠杆**，2c 不碰 selector，别把 2c 当它的解）。开工前先真机复测当前帧时间重估急迫性。

---

## 1. 关键结论：这是**转换**不是**重写**（相比 2026-07-17 大幅降风险）

2026-07-17 文档的架构调研（§1c）已过时（描述旧 QM-TIN 世界）。b20 northstar 工作后，现状已非常接近目标：

| 维度 | 2026-07-17 文档假设 | **b20 现状（实测）** |
|---|---|---|
| 地形源 | QM TIN；HeightmapProvider 是死代码 | **HeightmapTerrainProvider 已接线生产**（WebMercator PNG 高度图→`DecodedHeightmap{tileSize, heights[]}`），`EarthEngineSdkFacade.cpp:156-209` |
| 几何 | 无规则栅格渲染路径 | **规则栅格已是唯一路径**（`EllipsoidTerrainMeshBuilder::buildEllipsoidGrid`，`content/EllipsoidTerrainMeshBuilder.cpp:123-251`） |
| morph 起点 | 「最近可渲染祖先」→浮上来 | **已修=coarse-self**（本瓦片 2× 偶数子网格 bilinear 下采样烘进 `heightDelta@28`，`EllipsoidTerrainMeshBuilder.cpp:214-235`，零主线程、无父依赖） |
| morph 进度 | 定时器 | **已是每帧距离/SSE 连续**：`clamp((sseRatio-0.5)/0.5,0,1)`，`TileRenderPlanFinalizer.h:240-251` |
| shader morph | 待建 | **已在跑**：`pos += u_geomorphUpFactor.xyz * heightDelta * (1-morphFactor)`（GLSL `Renderer.cpp:925`、MSL `:2122` 一致） |
| 地形挂载 | — | **已是 Tileset**（`SceneTilesetCoordinator::primary_`，走 `Tileset::update`） |

**含义**：2026-07-17 文档 §0 要解决的「浮上来」**已被 coarse-self + 距离连续 morph 解决**（那份文档的问题定性已不成立）。当前 shader morph 公式**正是** §0.5 锁定的模型——只差**位置仍是 CPU 逐顶点烘焙**。**唯一剩余缺口 = 把位置从 CPU 烘焙（`EllipsoidTerrainMeshBuilder`→per-tile `TerrainGpuVertex`→per-tile VBO）改为 shader 采样（共享模板 + per-tile 高度纹理 + 着色器内椭球位移）。**

---

## 2. 参考基准：osgEarth REX 生产模型（`.ref/osgearth`，非凭记忆）

REX（当前生产引擎，非废弃 MP）的 GPU 地形**不是**「纯平面单位方格、全在 shader 算」，而是混合：

1. **共享模板 = 零高程、贴合椭球的网格**，按 `{LOD, tileRow, tileSize}` 缓存共享（`GeometryPool.cpp:138-145`）。即模板**已编织椭球曲率**（每纬度行烘一次），非扁平。顶点存**瓦片局部切平面坐标**（RTC，float32 安全）。`TileMesher.cpp:187-296`。
2. **GPU 位移**：`vertex.xyz += oe_UpVectorView * elev`，`elev = texture(oe_tile_elevationTex, uv).r` 反量化（`RexEngine.elevation.glsl:24-39`、`RexEngine.SDK.glsl:27-40`）。在 view 空间、per-tile MVM 之后。
3. **精度 = per-tile RTC**（核心）：每瓦片局部切平面帧（CPU 双精度算 `world2local` 一次），顶点 = 瓦片局部（米~km 量级，float32 安全），**per-tile MVM**（`local2world*view`，CPU 双精度→降 float32）折叠掉 6.3e6 的大数。GPU 只见 float32。`TileMesher.cpp:190-193`。
4. **法线**：CPU 解析椭球法线（模板顶点属性，共享，位移方向用）+ 独立 CPU 烘焙 normal-map 纹理（着色浮雕用，octahedral 编码）。**无 shader 内梯度**。
5. **morph**：osgEarth 用**第二顶点属性流** `a_neighbor`（偶数下标子采样，模板内烘一次）→ `mix(a_position, a_neighbor, morphFactor)`。**我们的「本纹理双分辨率采样」是合法简化**（省 a_neighbor 属性、无需嵌套金字塔），caveat 见 §5。
6. **skirt**：周边顶点复制、沿 -normal 挤出 `radius*ratio`，属共享模板。裂缝靠 morph+skirt 遮挡（非密度匹配）——与我们 [[coarse-tile-skirt-wall-exposure]] 一致。

**GPU Gems 2 几何 clipmap**：单一全局 clipmap 是平面/相机中心同心环，**不贴球**（极区/接缝/投影奇点）。**瓦片式（osgEarth/Cesium/GE 生产做法）才是球面正解**。唯一值得借的 clipmap 点：**把 fine+coarse 高度打包进一次纹理取样**（简化 morph 为单次 fetch）。

---

## 3. 目标架构（本引擎，参考验证）

```
DecodedHeightmap{65×65 uint16 + halo}  ──upload──▶  per-tile 高度纹理 (R16UI/R32F)
                                                          │
共享模板（per {LOD, mercator-row}）：                      │ 顶点 shader 采样
  零高程贴椭球规则栅格，瓦片局部 RTC 坐标 + 解析法线 ──────┼──▶ pos_local += normal * elev
  单份 VBO/IBO（含 skirt 环），全瓦片复用                   │     ↓ morph（本纹理双分辨率 或 a_neighbor）
                                                          │     ↓ × per-tile MVM uniform（折叠 ECEF）
per-tile uniform：MVM（double→f32）、高度纹理句柄、bounds  ──┘     → gl_Position
```

- **每瓦片 GPU 成本**：1 次纹理上传（65×65 R16 ≈ **8.5KB** vs 现在 65×65 `TerrainGpuVertex` VBO ≈ **135KB** + IBO）+ 若干 uniform。**无 CPU 建网格**（`buildEllipsoidGrid` 逐顶点 `cartographicToCartesian` 消失）。
- **morph 复用现有** distance-continuous morphFactor（`TileRenderPlanFinalizer`）+ shader mix 公式，仅换 morph 目标来源（heightDelta 属性 → 纹理双采样 或保留属性）。
- **共享模板粒度**：WebMercator 下同 `{LOD, row}`（同纬度带）瓦片形状全等 → 按行共享（osgEarth 同款）。可选进一步：切平面近似下同 LOD 全共享（形状仅纬度缩放，用 MVM 吸收），需验证高纬畸变。

---

## 4. 改动点（audit 定位的 8 处 + 精度/采样层）

| # | 文件:行 | 现状 | 改为 |
|---|---|---|---|
| 1 | `EllipsoidTerrainMeshBuilder::buildEllipsoidGrid` :123 | 逐瓦片烘 ECEF 位置+高度 | 一次性共享模板生成器（瓦片局部 UV/位置+解析法线），按 {LOD,row} 缓存 |
| 2 | `HeightmapTerrainContentProvider::buildContent` :218 | 传 heightSampler 建 CPU 网格 | 上传 `DecodedHeightmap.heights` 为 per-tile 纹理，不建网格 |
| 3 | `TerrainGpuVertex`/`buildTerrainVertices` :211 | per-tile 打包 | 地形不再逐瓦片打包；模板顶点只带 UV/grid-index |
| 4 | `GltfRenderResourcePreparer::uploadToGpu` :589 | per-tile VBO+IBO | 模板 VBO/IBO 建一次；per-tile 改 `createTexture`（已有纹理上传路径 :651-672 可仿）+ uniform |
| 5 | `kTerrainVertexGLSL`/`kTerrainVertexMSL` `Renderer.cpp:906/2097` | 用烘好的 pos | 采高度纹理 + 椭球位移 + 保留 morph；GLES3/Metal 均支持顶点纹理取样 |
| 6 | 法线 `EllipsoidTerrainMeshBuilder.cpp:183` | CPU 中心差分 | 模板解析法线（位移方向）；浮雕着色可选 CPU normal-map 纹理 |
| 7 | skirt `appendRegularGridSkirt` :260 | per-tile 生成 | 共享模板 skirt 环，随纹理一起位移 |
| 8 | morphFactor/uniform `TileRenderPlanFinalizer.h:240`+`GltfDrawCommandBuilder.cpp:344` | 已距离连续 | **基本原样复用**（只喂 w+up，独立于顶点来源） |
| 9 | 精度 | pos[3]f32 **已是 per-tile RTC**（`buildTerrainVertices` 双精度算 ECEF−origin 再降 f32；无 positionHigh/Low split）| **基本不动**：RTC 基建现成且正确，GPU 位移只把「面点+法线·elev」的加法从 CPU 挪进 shader（恒等式，f32 亚毫米）。P0 host 单测锁死 |
| 10 | 高度查询 `LoadedTerrainHeightSampler`（拾取/相机贴地/矢量贴地） | 遍历 CPU 地形网格三角形 | **改直接采 `DecodedHeightmap`**（更简单）；须保 heightmap 常驻 CPU |

---

## 5. 潜在问题（精度/性能/内存/扩展/接缝，诚实全列）

- **精度（2026-07-20 P0 代码审计后大幅下调，原判「最大风险」不成立）**：审计坐实两点——(a) **RTC 已存在且正确**：`TerrainGpuVertex.pos` 现**已是瓦片局部 RTC float**（`GltfRenderGeometryBuilder::buildTerrainVertices` 用 `f64(ECEF·contentTransform − tileOriginECEF)` 双精度算完再单次降 f32，`GltfRenderGeometryBuilder.cpp:225-230`；原点 = `makeModel` 的 `preferredLocalOriginEcef` 双精度）；**无 positionHigh/Low split**（那是 CPU-only `SurfaceVertex` 上采样用的死字段，不上 GPU）。MVP 每帧在 `SceneRenderCommandUniformUpdater` 双精度 compose 后单次降 f32。⟹「迁 RTC 是硬改动」**已不成立，RTC 基建现成且正确**。(b) **位移分解是恒等式非近似**：`cartographicToCartesian(lat,lng,h) ≡ surfacePoint(lat,lng) + geodeticSurfaceNormal(lat,lng)·h`，因 `geodeticSurfaceNormal` 只依赖 lat/lng、height 项在参考实现里就是线性叠加（`Ellipsoid.cpp:46-54,81-88` 实读证）。⟹ GPU 位移 `pos = 模板面点_local + 法线_local·elev` 与现 CPU 烘焙路径**代数等价**，唯一新增 f32 误差 = 单位法线·elev(≤~9km) 加到 km 量级局部向量 → **亚毫米级**。**P0 因此降级为纯 host 单测**（无需设备/shader：跨高纬最坏瓦片，f32 RTC-local 分解 vs f64 baked 参考，max<1m），shader 两后端 parity 留 P1 附带验。
- **morph 接缝（我们的简化的真风险）**：「本纹理 2× bilinear 当 coarse」仅当 coarse LOD 真实数据用同款 box 下采样才无缝；若各 LOD 独立生成（不同源分辨率/滤波）→ morph 边界可见缝。osgEarth 用**真 coarse 网格派生的 a_neighbor** 属性避此。**退路**：保留 `heightDelta` 属性（现有 coarse-self 烘焙，已无缝）当 morph 目标，只把**位置**上 GPU；即 morph 数据仍 CPU 烘（省不掉那点，但那不是瓶颈），位置省掉。**这是最稳的中间态**——见 §6 P1。
- **顶点纹理取样**：GLES3 保证顶点纹理单元（GLES2 不保）；Metal 原生支持。R16UI/R32F 两后端 OK。需确认 Adreno 730 真机顶点纹理取样性能（老 GPU VTF 可能慢）。
- **性能**：上传省 ~16×（8.5KB vs 135KB）+ 消灭 CPU 建网格（掠视 prefetchFill 8-29ms→~0）。但顶点 shader 加纹理取样（每顶点 1-2 次）；draw 数不变（每瓦片 1 draw，模板 instanced/rebind）。net 需真机实测（掠视 grazing preset 前后对比）。
- **内存**：per-tile 高度纹理常驻（65×65×2B≈8.5KB×工作集）替代 VBO；CPU 侧 DecodedHeightmap 须为高度查询常驻。共享模板单份。总体应降（[[cache-10x-compression-investigation]]）。
- **overlay UV（2026-07-17 忧虑，现已缓解）**：b20 地形已是规则栅格 + 均匀 UV（`texcoord01`），overlay 已适配规则栅格 → **不再是当年 TIN 逐顶点耦合的大坑**；共享模板 UV 天然均匀，overlay 迁移面小很多。仍需验证 upsampled/raster mapping 路径。
- **拾取/贴地/矢量**：`LoadedTerrainHeightSampler` 现依赖 CPU 地形网格；GPU 位移后无 per-tile CPU 网格 → 改为直接采 DecodedHeightmap（更快更简单，但是改动site；矢量贴地 [[vector-data-system-design]] 同理）。
- **偏离 cesium / golden**：位置改 shader 计算 → golden 逐像素对拍须重建基线（若 shader 数学与 CPU 一致，像素应一致，但浮点差异需容差）。AI_INDEX/测试大改。这是架构级重构。
- **skirt/法线**：共享模板 skirt 随纹理位移后，skirt 底边高度须一致（osgEarth radius*ratio 自缩放可借）。

---

## 6. 分阶段（每阶段二元验证；flag 切换 A/B，全程零回归）

> **📍 进度快照（2026-07-21）**：P0 ✅ / P1a ✅ / P1b ✅（commit 81aefb818+365711394+479d4d4a7，Stage A 贴椭球→Stage B 高度纹理 shader 位移，真机 relief 确认：同相机 A/B 一致 + 掠视 30.9→60fps）/ **P2 ✅（commit deb97c702，见下，morph 连续生长）** / GPU 路径无高度图瓦片回落 guard ✅（commit 7f747bbc2，预防上采样变平 + 删过时 Stage-A）。**几何侧地形生长（z0–z12 shader 位移 + 距离连续 morph）功能上已完整。** 剩余 P3/P4/P5 = 健壮性/收尾，非可见生长。⚠️ 残留:demo GPU ON→OFF 重瓦片视图 pool.reset() 悬垂句柄崩溃（demo-only 低优先）。
> **重要厘清**：地形几何**恒封顶 DEM native z12,不上采样**（`config.terrain.maximumZoom=12` + `decoupleImageryFromGeometry=true` + `terrainPageStore=true`,北极星纹理/几何解耦=有意设计,避免 z13+ 几何上采样空洞）→ **无 AncestorUpsample 瓦片进 GPU 路径**（四组近景 800m/450m/400m/近nadir 真机均 Zoom 12-12 坐实）;深缩放清晰度由影像侧 page store(Phase 2b,z14+ 贴 z12 面)承担,与几何生长正交。上述 guard 的"上采样变平"分支因此当前不触发=预防性。

**P0 精度地基 ✅ 已完成（2026-07-20，纯 host 单测，无需设备）**：`scaffold/tests/unit/geodesy/test_terrain_gpu_displacement_precision.cpp`（3 test PASS）。做法=跨 LOD(z3 粗→z12 native)×纬度(0/45/60/72/80°)×高程(0/9km) 的 65×65 栅格，逐顶点比 GPU 位移 f32 RTC 分解 vs 双精度 baked 参考。**实测**：candidate-vs-truth **全 <1m**（最坏 z3-45°-72°N-9km = **0.26m**，4× 余量）；与现 shipping f32 RTC 路径**同阶**（~1.5-1.9×，非质变）；**z12 native = 0.8mm**；elev=0 时 candidate≡current 逐值相等（证分解代数等价）。⟹ **精度轴已彻底 de-risk，RTC 基建现成、位移分解是恒等式**。残留 = shader 两后端 f32 parity（GLES/Metal 与 host f32 一致），并入 P1 附带冒烟。
**P1 GPU 位移（保留 CPU morph 数据，最稳中间态）**：共享模板 + per-tile 高度纹理 + shader 采样位移；**morph 仍用现有 heightDelta 属性**（已无缝）。位置上 GPU，morph 数据暂留 CPU。验证（战术修后重锚——急性卡顿已不是 P1 的靶）：**主靶 = 地形 VBO 字节从「随可见瓦片数线性」压到「共享模板单份 + per-tile 8.5KB 纹理」（§5 有界闸门）+ 峰值内存降**；掠视 grazing preset 前后帧时间不回退（战术修后基线 ~11-18ms）；出图与旧路径像素近似；glError=0；host 全绿。（prefetchFill 已被战术修压到 ~1ms，不再是 P1 的胜负手。）
  - **P1a ✅ 已完成（2026-07-20，纯 host，零改现有路径）**：共享位移模板模块 `content/TerrainDisplacementTemplate.{h,cpp}` + host 测试 `tests/unit/content/test_terrain_displacement_template.cpp`（3 PASS）。**关键设计坐实**：模板顶点存**瓦片中心 ENU 局部帧**（`ecefToEnu(center)·面点`），令同 {LOD,mercator-row} 不同列**逐值相等**（测试证 col 0/3/7/100 逐顶点 <1mm）→ 一份模板跨该行所有列共享（§5 有界 VBO 前提成立）；经纬度落位由 per-tile `enuToEcef(center)` 刚体帧承担（**这是把现 translation-only origin 升为全刚体 MVM 的设计根据**，P1b 落地）。重建 `frame·(localPos+localNormal·h)` vs `cartographicToCartesian` = f32-RTC 量级（z5 0.032m / z8 0.003m / **z12 native 0.3mm**，与 P0 同）。采样约定（纬 north→south、经 longitudeAt 反经线、缠绕 a,c,b,b,c,d）与 `EllipsoidTerrainMeshBuilder` 逐字对齐 = 等价替换。**feature flag 挪到 P1b**（现无消费者，避免 orphan 配置）。**下一步 P1b**：per-tile R16UI 高度纹理上传 + per-tile 全刚体 MVM uniform + flag 门控 draw。
**P2 morph 纹理化 ✅ 已完成（2026-07-21，commit deb97c702，真机验证）**：**不再是"可选"——P1b 落地暴露"§7.1 保留 CPU heightDelta"方案不成立**:共享模板按 {LOD,mercator-row} 复用,而 heightDelta 是 per-tile 数据,不能做模板顶点属性,故 P1b 只能把 heightDelta 清零(+w=1)=**硬 pop 无 geomorph**。⟹ morph 必须纹理化(非可缓)。做法=顶点 shader 本纹理双分辨率采样:`hFine`=texelFetch(本顶点栅格纹素),`hCoarse`=四偶数格点双线性(osgEarth 邻居平均:偶点=self→delta0,奇点=相邻偶点均值),`h=mix(hCoarse,hFine,morphFactor)`。morphFactor 复用已有 SSE 驱动的 `u_geomorphUpFactor.w`(finalizer 算、applyPerFrameCommandState 每帧盖章,无需改 builder)。GLSL(`Renderer.cpp` kTerrainVertexGLSL)+MSL(kTerrainVertexMSL)同步,各加 `eeSampleTerrainHeight` 反量化 helper。**真机验证(Adreno 7e045e39)**:fine 端点(settled w=1)与改前**逐字节一致**(mix(_,fine,1)=fine 数学恒等→零回归)、coarse 端点(w=0 临时 build)相干更平滑**无裂缝**、128 瓦片掠视多距离中间态不同 morph 进度瓦片间**无接缝**;glError=0、掠视 30.9→60fps。**§5 点名的 morph 接缝风险排除**(coarse-self 是 CPU 已验证 osgEarth 邻居平均的忠实 shader 移植;相邻瓦片共享偶点高度一致→无缝)。
**P3 高度查询迁移**：拾取/相机贴地/矢量贴地改采 DecodedHeightmap。验证：拾取准、相机不穿地、矢量贴合。
**P4 skirt/法线/normal-map + overlay 回归**：验证：无 LOD 裂缝、浮雕正确、overlay 贴合。
**P5 收尾**：QM/旧 CPU 网格路径退役或保留 flag、golden 重建、AI_INDEX+测试更新。

---

## 7. 决策（2026-07-20 已拍板 ✅）

1. ~~**morph 策略 = P1 保留 CPU heightDelta 属性**（已无缝，只把位置上 GPU）。纹理化 morph 留 P2，可缓。~~ **❌ 已推翻(2026-07-21)**:共享模板跨 {LOD,row} 复用,heightDelta 是 per-tile 数据无法做模板顶点属性 → P1b 只能清零 heightDelta=硬 pop。**morph 必须纹理化(P2,已完成 deb97c702)**,非可缓。见 §6 P2。
2. **共享模板粒度 = per {LOD, mercator-row}**（osgEarth REX 同款，稳；per-LOD 全共享留作后续优化）。〔推荐默认，未反对则采用〕
3. **高度纹理格式 = R16UI**（配 §0.6 raw uint16 数据源）。〔推荐默认〕
4. **范围 = flag-gated A/B**：GPU 位移路径与现有 CPU 网格路径运行时 flag 并存，可真机 grazing preset 前后对比、零回归；P1-P4 验证后再退役旧路径。✅
5. **法线 = P1 仅解析椭球法线**；CPU normal-map 浮雕后置（P4，观感提升）。〔推荐默认〕

## 8. 实施顺序（决策落地）
P0 精度冒烟(per-tile RTC 局部帧+MVM,单瓦片 GPU pos vs CPU ref <1m,两后端) → P1 GPU 位移路径(共享模板 per{LOD,row}+per-tile R16UI 高度纹理+shader 椭球位移,**保留 heightDelta CPU morph**,flag 并存) → 真机 grazing preset 前后对比(prefetchFill 8-29ms→~0/60fps) → P2 morph 纹理化(可缓) → P3 高度查询迁移 DecodedHeightmap → P4 skirt/normal-map/overlay 回归 → P5 退役旧路径+golden 重建。

## 相关
[[horizon-jank-terrain-tileset-2026-07-19]]（卡顿根因链+复现）、[[ge-loading-experience-gap-2026-07-17]]、[[terrain-continuous-lod-redesign-2026-07-17]]（被本文更新）、[[coarse-tile-skirt-wall-exposure]]、[[overlay-terrain-coupling-filmesh-2026-07-08]]、[[perf-measured-on-debug-build]]。
参考源：`.ref/osgearth`（REX 生产引擎，~数百MB LGPL，用完可删）、`.ref/cesiumjs`（TerrainFillMesh）。
