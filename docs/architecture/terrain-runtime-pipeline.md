# 地形运行时架构、策略与执行链路

> 本文回答“地形在一帧中如何从相机输入走到最终绘制”“目标 LOD、资源就绪和实际呈现如何解耦”“各项策略为何存在”。
>
> 地形内容格式、网格构造、GPU 位移和接缝算法的内部细节见 [terrain.md](terrain.md)；通用瓦片选择、请求调度和缓存机制见 [tiling.md](tiling.md)。本文以跨模块执行顺序为主，不重复展开两个子系统的全部实现细节。
>
> 文档基于当前仓库源码与本地测试。符号名是稳定引用，行号只作临时路标。

---

## 1. 目标体验与架构结论

地形系统的目标体验是：

- 相机移动、飞行、缩放和数据加载期间，地球表面持续可见，不因新瓦片尚未完成而露出背景；
- 细节随屏幕误差渐进提升，资源生产不能长时间阻塞主线程；
- 地形、影像、CPU 高程查询和实际绘制使用一致的瓦片身份与高度语义；
- 缺失、取消、临时失败和永久失败都有明确退化方向；
- 性能优化优先消除重复遍历、重复网格、无效请求和集中上传，不默认牺牲可见细节。

当前生产架构的核心结论是：

> 地形依附于统一的 `TilesetTile` 四叉树。选择、内容生命周期、缓存和渲染计划共享同一套瓦片身份；不存在另一棵独立的生产地形 LOD 树。

`TerrainDisplacementTemplatePool`、`TerrainPageStore`、高度纹理池和 `TerrainHeightService` 都是选择后的资源或查询设施，不会独立遍历并产生另一组可见地形瓦片。

总体结构如下：

```text
Engine / Scene
  └─ SceneFrameUpdateCoordinator
       ├─ CameraSystem + dynamic near plane
       ├─ SceneFrameStateBuilder
       └─ SceneTilesetCoordinator
            └─ Tileset
                 ├─ TileScheme / TilesetTileRegistry
                 ├─ TilePlan / selection reuse / quadtree traversal
                 ├─ TerrainProvider / content lifecycle
                 ├─ TileLoadQueue / pending results / GPU upload queue
                 ├─ cache / eviction / render references
                 ├─ TerrainHeightService
                 └─ render command preparation
                      └─ SceneRenderPipeline → Renderer::submit
```

核心源码入口：

- [`SceneFrameUpdateCoordinator.cpp`](../../scaffold/src/earth_engine/scene/SceneFrameUpdateCoordinator.cpp)
- [`SceneTilesetCoordinator.cpp`](../../scaffold/src/earth_engine/scene/SceneTilesetCoordinator.cpp)
- [`TilesetUpdateFrameRuntime.cpp`](../../scaffold/src/earth_engine/tiling/TilesetUpdateFrameRuntime.cpp)
- [`TileFrameWorkCoordinator.h`](../../scaffold/src/earth_engine/tiling/TileFrameWorkCoordinator.h)
- [`TileRenderPlanFinalizer.h`](../../scaffold/src/earth_engine/tiling/TileRenderPlanFinalizer.h)
- [`SceneRenderPipeline.cpp`](../../scaffold/src/earth_engine/scene/SceneRenderPipeline.cpp)

---

## 2. 关键对象与职责边界

### 2.1 `SceneFrameUpdateCoordinator`：生成可靠的本帧输入

它负责：

1. 更新 `CameraSystem`；
2. 根据相机与最近地形几何的距离调整透视 near plane；
3. 在构造 `FrameState` 前写入飞行和交互状态；
4. 构造选择器使用的视图、时间、环境和帧号；
5. 调用 `SceneTilesetCoordinator::update`。

动态 near plane 不只是视觉参数。过大的 near plane 会切掉近地山体；固定且不合适的 near plane 也会使 reverse-Z 的有效深度落入 float32 条件较差的区间。因此 near plane、地形高度探测和相机约束属于同一条体验链。

### 2.2 `SceneTilesetCoordinator`：管理主地形与接管

它区分：

- 当前 primary terrain tileset；
- 正在加载、等待接管的 pending primary tileset；
- 额外内容 tileset。

当前和 pending 地形可以同时更新。渲染时根据区域覆盖组合两者，待接管策略满足后再提交所有权切换，避免替换地形源时出现全屏空窗。

### 2.3 `Tileset`：统一状态和所有权

`Tileset` 聚合：

- 切片方案与根拓扑；
- `TilesetTileRegistry`；
- `TilePlan`；
- terrain/content provider；
- content lifecycle；
- 请求、pending result 和 GPU upload 队列；
- 帧资源预算；
- raster overlays；
- cache、驱逐和选择引用；
- `TerrainHeightService`；
- render command manager。

瓦片实体由 registry 持有。`parent/children` 表达拓扑，但选择状态、内容状态、渲染资源和派生内容都落在同一个 `TilesetTile` 身上。

### 2.4 `TilePlan`：当前帧意图，而不是资源所有权

`TilePlan` 保存：

- 可见瓦片 key；
- 选择阶段希望渲染的瓦片；
- 缺失内容请求；
- 最终 `TileRenderEntry`；
- 祖先回退、base-color fallback、drop reason 等诊断。

它表达“这一帧想画什么以及实际用什么覆盖”，但不拥有瓦片或 GPU 资源。

---

## 3. 一帧更新的精确顺序

### 3.1 场景入口

```text
Engine::render
  → Scene::update
  → SceneFrameUpdateCoordinator::update
      → CameraSystem::update
      → update dynamic near plane
      → write flight / interaction state
      → SceneFrameStateBuilder::build
      → SceneFrameResourceArbiter::beginFrame / declareDemand / sealAllocations
      → SceneTilesetCoordinator::update
          → primary Tileset::update          ┐
          → pending primary Tileset::update  ├─ share one Scene frame grant
          → additional Tileset::update       ┘
  → Scene::update MVT sources               # consume same frame grant
  → TerrainPageStore update/tick             # consume same frame grant
```

### 3.2 Tileset 帧工作

`TilesetUpdateFrameRuntime::run` 首先进入 `TileFrameWorkCoordinator::run`：

```text
begin FrameResourceBudget
  → process pending load results / upload work
  → classify selection reuse
  → reuse selection 或 full quadtree traversal
  → refresh/finalize render plan
  → raster mapping / upsample child materialization
  → request missing content
  → reconcile tile resources
  → optional base coverage pump
  → drain GpuUploadQueue
```

这里的 `FrameResourceBudget` 是 Tileset 局部保护，不再是全局总量。它附着到
Scene arbiter：局部 lane/inflight/毫秒预算先通过后，还要消费 terrain 或 raster
在对应 Scene stage 获得的 grant。这样 primary/pending/content Tileset 即使依次
更新，也不会各自获得一整份帧预算；MVT 与 PageStore 也不能在 Tileset 更新结束后
绕开对应的同帧 stage admission。需要区分：`WorkerDispatch` 与 `ComposeDispatch`
是独立 stage，虽可能进入同一底层线程池，却不是一个严格共享的线程池总上限；
`GpuUpload` 也按逻辑事务计费，而不是按字节、耗时或底层 GPU 调用次数计费。

需要特别注意三个顺序契约：

1. `processPendingLoads` 必须早于同帧 `drainGpuUploadQueue`；
2. 已完成内容先进入状态机，再做本帧选择，选择器才能看到最新 readiness；
3. 缺失请求在选择结果形成后规划，不能让网络状态反向决定拓扑遍历结构。

`processPendingLoads` 并不表示所有 GPU 操作都完成。它主要推进 worker/transport 已经产出的结果和主线程提交；真正放入 `GpuUploadQueue` 的工作随后由 `drainGpuUploadQueue` 消费。不同内容路径的线程占比并不完全相同，不能把整个地形管线简化成“全异步”。

---

## 4. 选择与 LOD 策略

### 4.1 选择输入

遍历一个瓦片前会计算或读取：

- frustum visibility；
- fog visibility；
- software/platform occlusion；
- camera-inside；
- 相机到包围体距离；
- 请求优先级；
- screen-space error；
- 祖先是否已经满足 SSE；
- 内容和影像 readiness；
- 相机是否正在移动、交互或飞行。

相关实现：

- [`TileSelectionInputMetrics.cpp`](../../scaffold/src/earth_engine/tiling/TileSelectionInputMetrics.cpp)
- [`TileSelectionCullingPolicy.cpp`](../../scaffold/src/earth_engine/tiling/TileSelectionCullingPolicy.cpp)
- [`TileSelectionRefinementPolicy.cpp`](../../scaffold/src/earth_engine/tiling/TileSelectionRefinementPolicy.cpp)
- [`TileSelectionTraversalExecutor.cpp`](../../scaffold/src/earth_engine/tiling/TileSelectionTraversalExecutor.cpp)

### 4.2 SSE 是目标细节，不是当帧呈现保证

SSE 使用实际投影矩阵把几何误差投影到屏幕，近似表达为：

```text
abs(project(errorOffset).ndcY - project(tileCenter).ndcY)
  × viewportHeight × 0.5
```

多视图时：

- SSE 取各视图最大值，避免任一视图细节不足；
- 请求 priority 取各视图最紧急值。

典型默认策略：

| 参数 | 默认值 | 作用 |
|---|---:|---|
| `maximumScreenSpaceError` | 16 px | 正常细化阈值 |
| `culledScreenSpaceError` | 64 px | 被裁剪区域的宽松误差阈值 |
| `maximumSimultaneousTileLoads` | 20 | 同时加载预算 |
| `loadingDescendantLimit` | 20 | 加载中后代限制 |

主要 refine 条件可以概括为：

```text
unconditionallyRefine
  || (!meetsSse && !ancestorMeetsSse)
```

SSE 只决定“选择器希望达到哪个 LOD”。如果该瓦片内容还没准备好，最终绘制阶段可以继续使用祖先资源。

### 4.3 Selection reuse

相机和资源状态稳定时，系统可以复用上一帧选择，避免重复遍历。复用分类会检查：

- 相机和视图是否发生影响选择的变化；
- terrain/content resource revision；
- raster overlay configuration signature；
- tileset 或 overlay 是否仍有 pending work；
- 是否处于 presentation hold；
- 交互后资源平滑窗口是否允许短暂陈旧结果。

复用只跳过昂贵选择计算，不跳过 pending 内容推进和 render entry 刷新。

---

## 5. DEM 内容生产链路

当前主要真实地形源是 Heightmap/Terrain-RGB 路径：

```text
HeightmapTerrainProvider
  → HTTP / cache / file
  → AsyncSystem worker
  → image decode + height decode
  → DecodedHeightmap
  → HeightmapTerrainContentProvider
  → EllipsoidTerrainMeshBuilder
  → terrain GltfModel / GltfPrimitive
  → TileContentLoadResult::renderTerrain
  → pending result
  → main-thread lifecycle commit
```

相关实现：

- [`HeightmapTerrainProvider.cpp`](../../scaffold/src/earth_engine/providers/HeightmapTerrainProvider.cpp)
- [`HeightmapTerrainContentProvider.cpp`](../../scaffold/src/earth_engine/content/HeightmapTerrainContentProvider.cpp)
- [`EllipsoidTerrainMeshBuilder.cpp`](../../scaffold/src/earth_engine/content/EllipsoidTerrainMeshBuilder.cpp)

关键数据契约：

- 高度语义是 WGS84 椭球高，单位米；
- 样本行从北到南；
- `u` 从西到东，`v=0` 在北侧；
- retained DEM 使用固定量化步长 `0.125m`；
- code `0`/数据源哨兵表示 no-data，不能等价为海平面；
- cell-registered 数据可以使用 `borderInset=0.5` 保持边界采样一致；
- 完整 `DecodedHeightmap` 被保留，供 GPU 位移和 CPU 高程查询共用；
- CPU 规则网格密度可以低于原始高度图分辨率。

解码得到真实 min/max 后会更新瓦片包围体。最低高度还要覆盖 skirt 范围，否则地形裙边可能被 frustum 或 occlusion 错误裁剪。

---

## 6. 主源、椭球与上采样策略

### 6.1 Composite provider 不是运行时失败重试器

[`CompositeTerrainProvider.cpp`](../../scaffold/src/earth_engine/content/CompositeTerrainProvider.cpp) 在请求之前根据 availability 选择 provider：

- 主源覆盖的瓦片使用主源；
- 只有整个 sibling quad 都不被主源覆盖时，椭球才填充纯空洞；
- 覆盖边界上的缺失 sibling 保持 `NotAvailable`；
- 边界缺失通过父地形上采样或祖先回退处理；
- 已选择主源后发生 HTTP 404、解码失败或永久失败，不会自动改投椭球。

目的在于避免覆盖边缘出现：

```text
真实高程 → 突然切换到 0 米椭球
```

这种断层比短期使用父地形更明显、更难隐藏。

### 6.2 两类派生内容

当前存在：

- `TerrainAvailabilityUpsample`：地形 availability 缺失时，由父地形派生；
- `RasterDetailUpsample`：影像希望继续细化但几何没有真实子内容时派生。

派生链路：

```text
父瓦片 glTF
  → 主线程建立不携带 TilesetTile 裸指针的快照
  → worker 裁剪父模型到子象限
  → 重建 UV / metadata / water mask
  → pending load queue
  → 统一 lifecycle commit / upload
```

上采样不占网络请求预算，但占用 worker、CPU 内存和后续资源准备预算。当 `decoupleImageryFromGeometry` 启用时，影像细节不再默认驱动几何上采样。

---

## 7. RenderPlan：目标瓦片与实际绘制分离

`TileRenderPlanFinalizer` 是防止加载期露洞的最后一道覆盖策略。

### 7.1 Direct entry

若选中瓦片本身具有可绘制资源，并满足基础影像 readiness，则：

```text
selectedTile == renderTile
```

### 7.2 Ancestor clip fallback

若当前瓦片不能直接绘制：

1. 沿父链寻找最近可渲染祖先；
2. 根据选中瓦片 bounds 计算祖先局部 clip UV；
3. 用祖先几何只覆盖当前子瓦片区域；
4. 当前瓦片资源准备好后再替换。

此时：

```text
selectedTile != renderTile
reason = AncestorFallback
```

这不是选择错误，而是目标 LOD 和资源 readiness 的正常解耦。

### 7.3 命令首建预算

新瓦片第一次建立常驻 draw command 可能产生主线程突刺。系统对交互期和恢复期设置不同的每帧首建预算：

- 预算足够：建立当前瓦片命令；
- 预算耗尽且祖先可覆盖：当前帧继续用祖先 clip；
- 没有任何祖先可覆盖：允许直接建立，优先避免露底。

因此预算只延迟细节，不应丢失覆盖。

### 7.4 Base-color fallback 与唯一允许的 drop

如果已有可绘制几何、只是基础影像尚未 ready，则仍生成 entry，由命令侧绘制 base color，而不是露出背景。

只有真正没有几何、也没有可覆盖祖先时才允许 drop。相关原因会被分类计数，区分：

- no geometry；
- no mapping；
- no ready texture；
- texcoord invalid；
- clip UV 失败；
- 其他资源不完整情况。

---

## 8. CPU 网格与 GPU 位移双路径

### 8.1 CPU baked fallback

每瓦片建立自身网格/VBO，适用于：

- GPU displacement 未启用；
- 模板池不可用或触顶；
- 上采样和部分非标准 primitive；
- GPU 高度层申请失败后的安全退化。

优点是路径直接、兼容性强；代价是 VBO 和上传成本随瓦片数增长。

### 8.2 共享模板 + GPU 高度位移

```text
共享零高程 ENU 模板
  + per-tile height texture layer
  + tile-specific ENU/ECEF frame
  → vertex shader displacement
```

[`TerrainDisplacementTemplatePool`](../../scaffold/src/earth_engine/tiling/TerrainDisplacementTemplatePool.h) 的策略包括：

- 同一 `{scheme, z, row, gridSize, geographic span}` 尽量共享模板；
- per-tile 高度存入 `texture2DArray` 的 layer；
- height layer 使用 epoch，模板 slot 使用 generation；
- LRU 淘汰后，常驻命令通过 epoch/generation 检测失效并重建；
- 可见瓦片每帧 touch，避免当帧资源被淘汰；
- 模板池或高度层失败时回落，不丢瓦片。

### 8.3 两档几何密度与迟滞

当前主要档位：

| 档位 | 网格单元数 | 进入条件 |
|---|---:|---|
| coarse | 64×64 | 正常或远景瓦片 |
| dense | 256×256 | tile SSE ≥ 64 px |

已在 dense 档的瓦片，SSE 降到 48 px 以下才释放回 coarse。`48–64 px` 是迟滞带，用来避免相机缓慢移动时逐帧升降档，反复重建命令和重烘高度纹理。

低层级还使用 `terrainReliefFade(z)`：

- `z ≤ 6` 基本压平；
- `z ≥ 9` 使用完整 relief；
- 中间平滑过渡。

这是远景表现策略，用于降低粗网格 faceting 和巨型 skirt 墙感，不是 LOD 选择策略。

---

## 9. CPU 高程查询链路

[`TerrainHeightService`](../../scaffold/src/earth_engine/tiling/TerrainHeightService.h) 建立在已加载的 retained `DecodedHeightmap` 上，不发起网络请求，也不生产新地形。

消费者包括：

- 相机碰撞和近地约束；
- picking；
- 矢量贴地；
- 高程范围计算；
- 调试探针。

查询策略：

1. 按 zoom 建立空间索引；
2. 优先选择最深的已加载瓦片；
3. 同 zoom 重叠样本取更高值；
4. 深层没有数据时退到祖先；
5. `heightmapGeneration` 变化时惰性重建；
6. 不规则 bounds 放入 fallback 列表。

CPU 查询若需要和实际渲染网格一致，应使用 `DecodedHeightmapSampler::sampleHeightRenderGrid`，避免矢量贴地高度与 GPU 所见表面分叉。

---

## 10. 请求、状态机与失败语义

请求主链：

```text
TileLoadQueue
  → priority sort
  → cache-key / inflight dedup
  → FrameResourceBudget
  → TileLoadRequestDispatcher
  → provider / transport / worker
  → pending terminal or upload queue
  → main-thread commit
```

重要策略：

- 请求去重，避免同一 cache key 重复在飞；
- 根据新旧目标请求集合取消 stale inflight；
- completion guard 保证同一请求只提交一次终态；
- 取消也必须进入终态，不能让瓦片永久停在 `ContentLoading`；
- 主线程加载工作受毫秒预算和最小推进数量共同控制。

终态语义：

| 结果 | 状态策略 | 目的 |
|---|---|---|
| `Success` | 提交内容并进入可用状态 | 正常路径 |
| `Empty` | 清理残留、标记完成并允许继续拓扑 | 空内容不是错误 |
| `RetryLater` | 临时失败并指数退避 | 防止每帧重打服务端 |
| `Cancelled` | 临时终态但不累计退避 | 调度取消不等于服务端失败 |
| `Failed` | 清理内容并永久失败 | 停止无意义重试 |

默认 `mainThreadLoadingTimeLimit` 为约 8 ms。交互期间与交互结束后的资源恢复期还会设置最小推进数量，避免严格时间预算让 pending 工作长期饿死。

---

## 11. 缓存、基础覆盖与资源保活

缓存总量不只包含地形 glTF/VBO，还需要考虑：

- retained DEM；
- raster textures；
- pending GPU upload bytes；
- terrain height texture layers；
- shared templates；
- 常驻 render commands。

主地形可以启用 base coverage：

- 初始种入低层级瓦片；
- 每帧限量推进基础覆盖影像；
- 已加载根层不参与普通预算驱逐。

目标是冷启动、快速移动或新数据源接管时：

> 可以先画低清晰度祖先，但尽量不要出现黑洞或背景透出。

渲染引用有严格生命周期：

- `buildRenderCommands` 为命令批增加引用；
- `Renderer` 使用帧级 keep-alive 保持裸 `Buffer*`/`Texture*` 有效；
- 正常帧必须在 `submit` 后释放 tile render references；
- hold/未提交帧必须显式丢弃未提交引用。

---

## 12. 最终渲染提交链

```text
SceneRenderPipeline::render
  → prepareTerrainOcclusion
  → buildSkyCommands / buildAtmosphereCommands
  → buildLayerCommands
      → Tileset::buildRenderCommands
      → TilesetRenderFrameExecutor
      → TileRenderFrameCoordinator
      → TileRenderCommandManager
      → TileRenderCommandPreparer
      → GltfDrawCommandBuilder
      → copy cached RenderCommand into frame list
  → TerrainDisplacementTemplatePool::flushHeightBakes
  → flushEdgeLutUploads
  → assembleTerrainBatches
  → applyMvpUniforms
  → sortAndValidate
  → runTerrainDepthPrepass
  → Renderer::submit
  → releaseRenderReferences
```

顺序理由：

- `buildLayerCommands` 会登记本帧需要的高度层和 LUT 更新；
- 高度 bake/LUT upload 必须在任何采样它们的 prepass/main pass 之前；
- batching 必须在 MVP 更新前完成，否则实例命令的 frame 不完整；
- terrain depth prepass 为后续符号遮挡提供当帧地形深度；
- 资源引用必须晚于 submit 释放。

当前没有单独的仓库级 `VertexArray/VAO` 所有权层；`RenderCommand` 直接引用 vertex/index buffer、纹理和相关地形元数据，因而调用顺序本身就是资源安全契约的一部分。

---

## 13. 策略总表

| 策略 | 目标体验 | 主要代价/风险 |
|---|---|---|
| 统一 `TilesetTile` 四叉树 | 地形、影像和选择共享空间身份 | content 与 surface readiness 状态较复杂 |
| 像素 SSE LOD | 以用户实际看到的误差控制细节 | 掠视/近地时遍历和请求压力上升 |
| Selection reuse | 稳定帧减少重复遍历 | 失效分类必须覆盖所有资源版本变化 |
| 祖先 clip fallback | 新瓦片加载时保持表面覆盖 | 暂时显示较低精度地形 |
| Base-color fallback | 影像未完成时不露黑洞 | 加载期可能显示简化颜色 |
| Base coverage preload/pin | 冷启动和快速移动先有地球表面 | 占用基础网络与缓存预算 |
| Composite pure-hole ellipsoid | 填补源范围外的大空洞 | 不处理主源运行时失败 |
| 父地形上采样 | 覆盖边缘不断层 | 精度受父瓦片限制 |
| 请求去重与 stale cancel | 消除无效网络/解码工作 | 需要严格终态和 exactly-once 保护 |
| RetryLater 指数退避 | 防止失败风暴 | 恢复会被延迟 |
| 主线程毫秒预算 | 控制帧时间突刺 | 内容完成到上屏可能跨多帧 |
| 命令首建预算 | 平摊新瓦片 command build burst | 细节替换延后一帧或多帧 |
| 共享模板 + 高度纹理 | 减少重复 VBO 与上传 | epoch/generation/LRU 管理复杂 |
| coarse/dense 迟滞 | 只给真正受几何 cap 的近景加密 | 有两套高度 array 和换档成本 |
| relief fade | 远景避免粗网格尖刺/裙墙感 | 低层级真实 relief 被压低 |

---

## 14. 性能模型与诊断方式

地形性能不能只看“可见瓦片数”。至少应分解为：

```text
输入/相机更新
  → FrameState
  → selection traversal
  → request planning
  → transport / IO
  → image + DEM decode
  → mesh/upsample CPU work
  → pending result commit
  → GPU upload / height bake / LUT upload
  → command first-build / cache validation
  → batching / sorting
  → depth prepass
  → main render submit
```

应关注：

- FPS 与 P50/P95/P99 帧时间；
- selection traversal 和 refine 时间；
- load queue、inflight、pending terminal、pending upload 数量；
- 每帧请求、取消、解码、commit 和上传数量；
- pending GPU bytes；
- template/height layer resident、eviction、epoch miss；
- first-build deferred 和 ancestor fallback 数量；
- terrain depth prepass 与主 terrain pass GPU 时间；
- retained DEM、纹理和 VBO 内存；
- Android 主线程阻塞、功耗和温升。

单一日志信号通常没有足够判别力。例如 `surface=0` 可能来自：

- selection/culling/BV/near-plane；
- 当前资源未 ready；
- render-plan fallback 无祖先；
- command 首建预算；
- template/height layer 失效；
- render validation 或 submit 路径。

最小有效实验是在固定相机、固定数据源下逐帧关联：

```text
selectedKey + SSE
renderKey + fallback reason
content/raster readiness
request/decode/commit/upload counters
template slot + height layer residency
selection/build/prepass/submit timing
```

---

## 15. 竞争架构假设与反证

### H1：统一四叉树地形流水线

**结论：支持，置信度高。**

支持证据：

- `Tileset` 同时拥有 registry、selection plan、provider、content lifecycle 和 render manager；
- selection 直接写 `TilesetTile::selectionFrameState`；
- 地形请求与内容状态绑定在同一 `TilesetTile`；
- `TileRenderEntry` 的 selected/render 两端仍是 `TilesetTile`。

最小证伪方式：找到独立于 `TilesetTileRegistry` 的生产地形 LOD 树，证明它自行遍历并在渲染阶段才与 surface tiles 空间组合。

### H2：独立 Terrain LOD 树

**结论：拒绝。**

容易误判为独立树的对象包括模板池、高度纹理池、page store 和高程服务。但它们都消费已选择或已加载瓦片，不能产生独立 selection。

### H3：同步选择 + 异步资源生产

**结论：支持，但需限定。**

跨帧队列、worker 解码、pending commit、上传预算和祖先回退明确存在；但不同 terrain 内容路径仍包含主线程 prepare/upload 工作。是否成为瓶颈必须用设备 trace，而不能由类名或注释推断。

---

## 16. 当前验证与未验证边界

聚焦 native 测试已验证以下局部契约：

```text
test_heightmap_terrain
test_composite_terrain_provider
test_tile_selection_input_metrics
test_tileset_selection_refinement
test_tile_render_plan_finalizer
test_terrain_height_service
test_terrain_displacement_template
test_decoded_heightmap_sampler
test_selector_cesium_golden_diff
```

结果：`9/9` 通过，总耗时约 `11.58s`。

覆盖范围包括：

- DEM 解码与地形模型；
- Composite provider availability/fallback；
- SSE 与 refinement；
- ancestor render fallback；
- CPU 高程索引和采样；
- GPU displacement template 的 host 侧契约；
- 本地选择器 Cesium-style golden diff。

尚未覆盖：

- 真实网络到真实 GPU backend 的完整端到端链路；
- Android/GLES 或 Metal 的压力场景截图与 trace；
- 真实冷启动、快速飞行、缓存抖动下的 P95/P99 上传行为；
- 功耗、温升和长期内存峰值；
- `heightmapMaxNativeZoom` 等配置字段是否完全进入生产决策；
- 主源范围内 HTTP 404 时，产品是否希望保留永久失败语义或增加新的运行时 fallback 策略。

项目规则要求优先对照 `/Users/ldy/Desktop/work/cesium-native` 和必要时的 OpenGlobus 源码。生成本文时这两个参考仓库在当前环境不可用，因此本文对 Cesium 的表述仅指本仓代码中的对齐声明、结构和 golden diff 测试，不能替代对上游当前源码与 `test/Test*.cpp` 的直接核验。

---

## 17. 修改和排障入口

| 目标 | 首要入口 |
|---|---|
| 改 Scene→Tileset 更新顺序 | `SceneFrameUpdateCoordinator`、`SceneTilesetCoordinator` |
| 改一帧资源推进/选择顺序 | `TilesetUpdateFrameRuntime`、`TileFrameWorkCoordinator` |
| 改 SSE、可见性或 refine | `TileSelectionInputMetrics`、`TileSelectionCullingPolicy`、`TileSelectionRefinementPolicy` |
| 改请求预算/取消/终态 | `TileLoadScheduler`、`TilePendingRequestState`、`TileTerminalLoadPolicy` |
| 接新 DEM 地形源 | `HeightmapTerrainProvider`、`TilesetContentProvider`、`TileContentLoadResult::renderTerrain` |
| 改覆盖外椭球策略 | `CompositeTerrainProvider` |
| 改父地形派生 | `GltfTerrainUpsampler`、`TileGltfTerrainUpsampledChildMaterializer` |
| 改祖先回退和 base-color fallback | `TileRenderPlanFinalizer` |
| 改共享模板/高度纹理 | `TerrainDisplacementTemplatePool`、`GltfDrawCommandBuilder` |
| 改 CPU 高程查询 | `TerrainHeightService`、`DecodedHeightmapSampler` |
| 改 terrain submit 顺序 | `SceneRenderPipeline`、`TilesetRenderFrameExecutor` |

修改任何一个阶段时，应同时检查上下游契约。例如调整 selection readiness 后，必须检查 render-plan fallback；调整 height texture 生命周期后，必须检查 command cache 的 epoch 校验和 submit 前 flush 顺序。

---

## 18. 一句话记忆模型

```text
选择器决定“应该看到哪一级”，
内容系统决定“哪一级已经准备好”，
RenderPlan 决定“这一帧实际用谁覆盖”，
渲染资源系统决定“怎样以可控成本画出来”。
```
