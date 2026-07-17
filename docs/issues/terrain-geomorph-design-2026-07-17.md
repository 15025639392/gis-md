# 地形 Geomorph（几何渐变）设计 — 2026-07-17

## 目标

根治 LOD 过渡的 **ghosting（双影）+ pop（硬跳）**。当前 cross-fade（commit e3560e0cb）修掉了黑透，但淡入期两层不重合几何半透明叠加→ghosting，用户判定碍眼。geomorph = 让子瓦片顶点高度从「粗起点」平滑长到「真实高度」，**全程只渲染子瓦片一层**（父瓦片不同时画）→ 无双影、无 pop。cesium-native 对 QM 是硬切+skirt，无 geomorph 先例；heightmap 规则网格 geomorph 不适用 TIN。这是新工程。

## 核心机制（两变体共享）

- 每子顶点存 `heightDelta = 粗高度 − 真实高度`（标量，沿椭球法线方向）。
- 顶点 shader：`pos_morphed = pos + u_tileUp * heightDelta * (1 − morphFactor)`，morphFactor 0→1。
  - morphFactor=0：顶点在粗高度（≈父面/平滑态）；=1：真实高度。
- `u_tileUp` = **瓦片中心椭球法线**（per-tile uniform，非 per-vertex——瓦片跨角小，法线近似常量，省一个顶点属性）。
- **morphFactor 复用 `lodTransitionFadePercentage`**：调研证实刚 refine 出的子瓦片该值正好 0→1 over `lodTransitionLength`(~1s)。已流到 fragment(`u_renderOpacity`)，geomorph 要送进 **vertex** uniform（GLES 免费=共用 uniform 表加一条；Metal 需新增 setVertexBytes + MSL 签名加参）。
- **geomorph 取代 cross-fade 双层**：geomorph 是单层，父瓦片可在子就绪后立即移除（不需 fadingOut 不透明基底）。二者互斥——terrain 开 geomorph 时关 cross-fade 双层逻辑。cross-fade 修复保留给非 geomorph 内容/兜底。

## 两个变体（粗高度来源不同）

### 变体 A：精确 geomorph（morph 起点=父网格采样高度）
- `粗高度 = 父瓦片网格在子顶点 (u,v) 处插值高度`。起点精确落在父面→与父瓦片无缝衔接（GE 级）。
- **代价**：
  - 需给每个子顶点采样父网格（`LoadedTerrainHeightSampler::sampleGltfTerrainHeight` 逐三角形暴力扫描，无空间索引）→ O(子顶点×父三角形)，慢；需 `LoadedTerrainAreaSampler` 式「先筛父候选再批量采样」优化。
  - **跨线程张力**：父网格只在**主线程**可读（`renderContent.gltfModelForRead()`），顶点构建在 **worker**（`buildTerrainVertices`）→ 需 `cloneParentForClip` 式主线程快照喂 worker。
  - **父存活**：构建期父 CPU 网格存活保护当前**只覆盖上采样子瓦片**（`derivesTerrainFromParent()`）；普通 Replace-refine 子瓦片无同等保护，父存活靠 referenceCount（过渡期被渲染时才 alive）——需补存活保证。
  - ⚠️ 上采样子瓦片本身=父网格裁剪产物（高度=父高度插值），无高度差→**不需要 geomorph**；geomorph 只对真实服务器细瓦片有意义，恰是父存活保护缺失场景。

### 变体 B：自平滑 geomorph（morph 起点=子网格自身 Laplacian 平滑高度）★推荐先做
- `粗高度 = 子网格顶点的 Laplacian 平滑值`（= 网格邻居高度平均，worker 内自足，无需父访问）。
- `heightDelta = 真实高度 − 自平滑高度`。morph 时细节从「平滑态」平滑长出。
- **优点**：完全 worker 内自足——无父访问/无跨线程快照/无父存活问题/无采样性能问题。工程量远小于 A。
- **代价**：起点是「子的平滑版」非「父的精确面」，二者有小残差（都逼近同一地形→接近但不完全等）→ 换手瞬间可能有极小 seam。质量 ~80% of A，工程 ~40%。
- 平滑强度可调（Laplacian 迭代次数/权重）逼近「粗一级 LOD」观感。需从三角形索引建顶点邻接表（worker 内一次）。

## 改动面清单（两变体共享，除粗高度计算）

1. **保留高度**：QM 解析出海拔后转 ECEF 即丢（`SurfaceVertex` 无 height 字段）。需透传 height 到 `buildTerrainVertices`，或反解 `cartesianToCartographic`（极点退化风险，`QuantizedMeshParser.cpp:540` 有警告）。变体 B 只需相对高度差，可从顶点 pos 沿 up 投影得到，规避绝对海拔。
2. **顶点格式 28→32B**（`GltfRenderGeometryBuilder.h:41-69` + static_assert）：加 `float heightDelta`。
   - ⚠️ **GLES stride 分派冲突**：`RenderDeviceGLES.cpp:743-751` 按 stride 数值路由，32B 会撞 Surface32（也是 32B）。需改用布局种类而非纯 stride 分派，或给 TerrainCompact 一个不冲突的判别。
   - GLES 属性录制 `recordVaoLayout` TerrainCompact 分支加 attrib slot（避开 instance 矩阵占用的 3-9；terrain 非实例化，slot 3 实际空闲但需确认）。
   - Metal descriptor `RenderDeviceMetal.mm:479-493` Terrain 分支加 `attributes[3]=Float @24 stride32`（Metal 按 shader 名分派不撞 stride）。
3. **buildTerrainVertices**（`GltfRenderGeometryBuilder.cpp:211-247`）：算并填 `heightDelta`；变体 B 在此做 Laplacian 平滑（需邻接表）；需 ellipsoid 引用算 tile-center up（当前无此依赖，调用方传入）。
4. **预建字节链**：`terrainGpuVertexBytes` 生产(`QuantizedMeshContentLoader.cpp:273`)/消费(`GltfRenderResourcePreparer.cpp:408`)/校验(`TilesetContentLifecycleCoordinator.h:176`)全用 `sizeof(TerrainGpuVertex)`，自动跟着变；上采样路径 `TileGltfTerrainUpsampledChildMaterializer.h:209-324` 同样，需验证。
5. **morphFactor 到 vertex uniform**：
   - GLES：`GltfUniformBlock` + `kGltfUniformTable`(`GltfUniformBlock.h:180`) 加 `u_morphFactor`（或复用 `u_renderOpacity`），通用循环自动上传；GLSL vertex shader(`Renderer.cpp:880-901`)加 `uniform float u_morphFactor` + morph 计算 + `uniform vec3 u_tileUp` + `in float a_heightDelta`。
   - Metal：vertex 函数(`Renderer.cpp:2010-2038`)当前只收 buffer(1) MVP，需新增 setVertexBytes（`RenderDeviceMetal.mm:833`）绑 morphFactor+tileUp，MSL 签名加参 + `TerrainVertexIn` 加 `heightDelta [[attribute(3)]]`；`GltfUniformBlock` MSL 镜像三方契约同步（`GltfUniformBlock.h:14` 注释）。
6. **tile-center up uniform**：per-tile 椭球法线，`GltfDrawCommandBuilder.cpp:318` 附近写入（从 tile 中心 ECEF 算 `geodeticSurfaceNormal`）。
7. **skirt**：skirt 顶点在同缓冲末尾追加，无 per-vertex GPU 标志（`SkirtMetadata` 是 CPU-only range）。geomorph 时 skirt 应跟随 morph（heightDelta 同法计算即可自然跟随），需验证不产生裙墙缝。
8. **geomorph↔cross-fade 互斥**：terrain 开 geomorph 时，`TileRenderPlanFinalizer` 不再走 fadingOut 不透明基底（父就绪即移除）；需一个开关区分 geomorph vs cross-fade 模式。

## 潜在问题（精度/性能/内存/扩展性）

- **精度**：变体 A 反解海拔极点退化；变体 B 平滑起点与父面残差。tile-center up 近似在大瓦片/高纬度略偏（可 per-vertex up 但+内存）。
- **性能**：变体 A 父采样 O(顶点×三角形) 是真实瓶颈；变体 B Laplacian 平滑 O(顶点+边) 一次性 worker 内，廉价。
- **内存**：顶点 28→32B = +14% 顶点带宽（与之前 40→28B 压缩部分抵消）。
- **扩展性**：coarsen（zoom out）方向——父替换子，父需 morph from 子高度。变体 B 父自平滑同样适用；变体 A 需采样 4 个子（更复杂）。**建议 coarsen 先不做 geomorph**（去细节方向观感不敏感），仅 refine 方向。
- **上采样子瓦片**：无高度差不需 morph，heightDelta=0 自然 no-op，但要确保不误算。

## 可验证信号（目标驱动）

1. 机制：新增 `morphTiles>0` 打点证 geomorph 激活；`heightDelta` 非零顶点占比。
2. 正确性（单测）：native 全绿；给定父/子高度构造 heightDelta，验 morphFactor=0→顶点在粗高度、=1→真实高度（顶点 shader 逻辑可在 CPU 镜像测）。
3. 像素（真机主观，归用户）：LOD 切入**无双影、无 pop**，细节平滑长出；skirt 无缝。
4. 性能（release）：Tileset.update / prepareCpuWork 不因 heightDelta 计算显著上升（变体 B 应可忽略）。

## 推荐路线

**先做变体 B（自平滑）**：无父访问的架构风险、worker 内自足、~80% 质量、~40% 工程。跑通全链（顶点格式+shader morph+morphFactor 复用+skirt）后真机看观感。若「平滑起点 vs 父面残差」肉眼可见有 seam → 再升级变体 A 的父采样（届时基础设施已就位，只换粗高度来源）。

分步（每步可验证）：
1. 顶点格式 +heightDelta（先填 0，验 28→32B 全链不崩 + stride 冲突解决 + 两后端上传正常）→ native 绿 + 真机无回归。
2. shader morph + morphFactor 复用 + tile-center up（heightDelta 仍 0，morph 为 no-op，验 uniform 链通）。
3. 变体 B Laplacian 平滑算 heightDelta（worker，邻接表）→ 真机看 geomorph 观感。
4. skirt 跟随 + geomorph↔cross-fade 互斥开关。
5. coarsen 方向（可选，观感不敏感可延后）。

## 2026-07-17 更新：变体 B 已上真机 → 因「多次从 0 生长」升级变体 A

**变体 B 真机结论**：碎裂/裂缝护栏都成立，但暴露结构性天花板——每层瓦片自平滑出的「起点平版」与屏幕上**上一层显示的地形对不上**，且每次 refine 都重置。放大穿越多层 LOD 时表现为地形「反复塌回平版再长起来」（用户报「多次从 0 生长」）。这不是调参问题，是变体 B 的天花板（起点与父面无绑定）。

**升级变体 A（父级采样）——已实施**，与原文档设想的「worker 内父采样 O(顶点×三角形) 是瓶颈」不同，落法规避了那个瓶颈：

- **主线程补丁而非 worker**：`TileGeomorphHeightDelta::applyParentGeomorph(child)` 在 CPU-ready→GPU upload 缝（`TilesetContentLifecycleCoordinator` lambda）里跑，主线程、父瓦片仍在注册表活着 → **无需 worker 传父模型快照/生命周期机制**（原文档担心的父访问架构风险直接消解）。
- **网格加速器破 O(v×t)**：`ParentHeightGrid` 把父 TIN 三角形按经纬度均匀网格分桶（格数≈√三角形数），每次采样 ~O(1) → 整体 O(父三角形 + 子顶点)，主线程一次性可接受（非逐帧）。
- **就地补丁 heightDelta**：只重写 `terrainGpuVertexBytes` offset 28 的 float32，shader/uniform/morphFactor/skip-fadingOut 全不动（变体 B 的接线完全复用）。
- **buildTerrainVertices 简化**：删掉 Laplacian 自平滑块，worker 侧 heightDelta 恒 0，morph 起点全权交主线程父采样。
- **边缘/无覆盖**：父级采样 miss（瓦片边缘/无数据）→ delta=0=该顶点不 morph 贴真值 → 跨瓦片边界天然对齐，替代变体 B 的显式 boundary 锚定护栏。
- **门控**：`TileRenderContentState.geomorphHeightDeltaApplied`，每次 `prepareGltfContent` 换模型复位，幂等。
- **根瓦片/父级无网格**：不 morph（heightDelta 0，直接出现，最粗层可接受）。

未决：真机验证「多次从 0 生长」是否消失、morph 是否连续接上一层；主线程父采样耗时真机插桩（预期廉价，网格摊平后）。
