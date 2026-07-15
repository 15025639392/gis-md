# 平台无关层技术债审计——正确性 / 性能 / 优雅度（2026-07-05）

**分支**：`codex/surface-instancing-gpu-batch`
**方法**：13 路并行只读审计（按子系统 × 维度切分约 7.5 万行平台无关层），每条发现由一名对抗性审核员独立读码核实（P0/P1 用高推理强度），结论分 confirmed / downgraded / refuted / duplicate-known。
**范围**：`scaffold/src/earth_engine/` 除 `platform/` 外全部（tiling / content / providers / core / renderer / scene / camera / interaction / environment / layers / data / style / sdk / debug / Engine）。
**不含**：`platform/` 后端（已由 2026-07-04 平台层性能审计覆盖）；工作区进行中的 P2-7 修复文件（TilePendingLoadQueue、CurlMultiRequestScheduler）；此前两轮审计的已修/已证伪/有意设计/已知在册项（已作为排除清单喂给每个审计员与审核员）。

## 0. 总结论

**85 条候选发现，验证后 63 confirmed / 13 downgraded / 1 duplicate-known / 7 refuted。没有 P0 存活——架构级病灶（uniform string-map、Metal 31-buffer、StreamingSet 假缓存、curl 唤醒、上采样 bounds 毒化等）在前几轮已全部修完。** 本轮剩余债务收敛为三个主题：

1. **正确性——两类真实可复现缺陷 + 一批潜伏/对抗性输入缺陷**。真实可复现的是 **GPU 资源生命周期泄漏**（`.release()` 转裸指针 + dispose 只置空，每次 surface 重建泄漏）和 **一处跨线程无锁数据竞争**（GoogleMapTiles 可用性向量）。潜伏类是 **递归无深度上限的栈溢出**（三处：glTF 节点树、tileset.json、含结构化元数据的按顶点 map 放大）和 **core/cache 的两个 UB/记账 bug**（deque 迭代器悬垂、put 字节虚增）——后者因模板尚未实例化而未爆，但一旦接线即触发。

2. **性能——常驻命令缓存落地后的"回吐"与遍历冗余**。P0-4 常驻缓存把构建成本降下来了，但 **每帧仍对每 primitive 全量深拷贝 RenderCommand**（stableKey 串 + textures vector 堆分配），把节省部分吐回；**SceneRenderPipeline 每帧对 1664 字节的命令列表做 7-8 遍全量扫描**（多数是无门控的诊断统计）；深缩放路径 **上采样每个子瓦片白拷 ~1MB 父地形顶点后立即丢弃**。其余是选择遍历、投影、availability 上的常数因子冗余（OBB 重建、逐顶点测地转换、morton 逐位循环）。

3. **优雅度——死代码是压倒性主题**。至少 **9 处成规模死代码**（合计约 1400+ 行）：`GltfRenderResourcePreparer` 两个 CPU-work 函数（~230 行）、`Uri` 类（~340 行，仅单测用）、`PersistentCache` 全家 + HttpCache 磁盘持久化（`setCacheDir` 从不被调）、`Future::then()` 虚假抽象、`TileTextureCache`（~173 行）、core/geodesy 测地线簇、死 MSL shader、只写不读的状态字段、恒报 0 的诊断计数。叠加 **4 个上帝文件**（GltfModel 8981 / GltfContentProvider 4267 / RasterOverlayTileProvider 3724 / Renderer 2622——后者 86% 是内嵌 shader 字符串）和 **分层倒挂**（core→tiling 反向依赖、6+ 组双向 include 环）。

**优先修复次序（ROI）**：先修两条真实可复现正确性（泄漏 + 数据竞争，各 <1h，手术式）→ 再补三处递归深度上限（防对抗性输入崩溃）→ 性能三连（RenderCommand 拷贝收敛、SceneRenderPipeline 遍历合并、upsampler 元数据浅拷）→ 死代码清理（低风险高收益，纯删/纯移，过 140 测试即安全）→ 上帝文件拆分与分层消环（整体重构，单列任务）。

---

## 1. 正确性

### P1

- **GPU 资源泄漏：SkyBox / AtmosphereBackgroundPass 用 `.release()` 转裸指针，dispose 只置空不 delete**
  [SkyBox.cpp:229](../../scaffold/src/earth_engine/environment/SkyBox.cpp#L229)（+253 vertex buffer，cubemap 模式还漏纹理）、[AtmosphereBackgroundPass.cpp:281](../../scaffold/src/earth_engine/environment/AtmosphereBackgroundPass.cpp#L281)（+301 buffer，审核降为 P2）。
  成员是 `ShaderProgram*/Buffer*/Texture*` 裸指针，`createShader(desc).release()` 拿走所有权，`dispose()` 只把成员置 `nullptr`。`Scene::setRenderDevice → environment_->initializeRenderResources → initialize()` 在每次 `onSurfaceCreated` 无条件重建 → 每次泄漏 1 shader + 1 buffer。Android 反复 context lost 时 GPU 内存稳定上涨。**修复**：成员改 `unique_ptr` 持有，dispose 用 `reset()`；initialize 幂等化（重建前先释放）。注意 `onSurfaceDestroyed` 路径不调 dispose，需在 initialize 开头也释放旧资源。

- **跨线程无锁数据竞争：GoogleMapTiles 可用性向量网络线程写、渲染线程读**
  [GoogleMapTilesImageryProvider.cpp:605](../../scaffold/src/earth_engine/providers/GoogleMapTilesImageryProvider.cpp#L605)。
  `requestTile` 的 `onViewport` 回调直接在 HTTP 传输线程内联执行（不像 XYZ 那样 defer），对 `availableRanges_/completeAvailabilityRanges_` 做 `push_back`；同时渲染主线程每帧经 `getTile→supportsTile→tileInRanges` 遍历这两个向量。无任何互斥。push_back 扩容重分配时并发遍历 → use-after-free / 撕裂读 / 崩溃。真机初次进入含 Google 影像视口时高概率复现。**修复**：新增 `availabilityMutex_` 覆盖两向量全部读写点（`addAvailableTileRanges/addCompleteAvailabilityRanges/isTileKnownAvailable/isTileInCompleteAvailabilityRange/hasKnownAvailability`）。注：单靠"defer 到 AsyncSystem::run"不够——`run` 派发到池线程而非主线程，仍需加锁。

- **无深度上限递归：glTF 节点树遍历栈溢出**
  [GltfModel.cpp:8151](../../scaffold/src/earth_engine/content/GltfModel.cpp#L8151)（`traverseNode`）、5298（`resolveNodeGlobalTransforms`）。
  `validateNodeHierarchy` 只保证森林结构（无环、单父），不限链长；N 个节点排成线性链完全合法。约 10 万节点线性链 → 递归 10 万层 → SIGSEGV。讽刺的是验证函数自身用显式堆栈避免溢出，验证通过后真正遍历却溢出。**修复**：改显式堆栈迭代遍历（复用验证函数的同款栈），或验证阶段设最大深度上限提前拒绝。

- **core/cache 潜伏 UB：SharedAssetDepot 用 deque 存 LRU 却缓存 deque::iterator**
  [SharedAssetDepot.h:189](../../scaffold/src/earth_engine/core/cache/SharedAssetDepot.h#L189)。
  `lru_` 是 `std::deque<string>`，`lruMap_` 存其 iterator。deque 的 push_front/pop_back 使**所有迭代器失效**。`touch()`/`put()` 一旦 push_front，`lruMap_` 里其它所有 key 的 iterator 全部悬垂，下次 `lru_.erase(悬垂it)` 即 UB。HttpCache 用 `std::list` 存 iterator 是正确的，此处是同一 LRU 模式的错误复刻。当前模板未实例化（潜伏），一旦使用即触发。**修复**：`lru_` 改 `std::list<string>`，或用 `list::splice` 实现 touch。

### P2（确认）

- **递归无深度上限：`parseTilesetJson` 的 `parseTile` 对 children 无限递归** — [GltfContentProvider.cpp:4133](../../scaffold/src/earth_engine/content/GltfContentProvider.cpp#L4133)。加载攻击者可控/损坏的 tileset.json，root.children 嵌套数千层即栈溢出。不受信任网络输入的 DoS/崩溃面。**修复**：`parseTile` 加最大递归深度常量（512/1024），超限该子树失效而非崩溃。
- **越界读：`blitImage` 的 RGB源→RGBA目标缩放回退路径用目标步幅读源缓冲** — [RasterOverlayTileProvider.cpp:1231](../../scaffold/src/earth_engine/providers/RasterOverlayTileProvider.cpp#L1231)。当 `platformBridge_->decodeImage` 返回 3 通道 RGB 而目标为 RGBA 且尺寸不同需缩放时，源指针步进用了目标 `bytesPerPixel` → 错位读源 + 可能越界。**修复**：源步进改用 `source.channels*source.bytesPerChannel`，通道拷贝源/目标分量各自计算（对齐 `unsafeBlitImage` 的失配路径）。
- **异常路径缺失：解析不受信任 TMS XML 数值属性抛异常且工厂无 try/catch** — [TileMapServiceUrl.cpp:185](../../scaffold/src/earth_engine/providers/TileMapServiceUrl.cpp#L185)。结构合法但含非法数值的 `tilemapresource.xml` → 配置期抛异常上传至 facade → 终止初始化。**修复**：`createTileMapServiceImagerySource` 包 try/catch 当作"无可用 tileset"返回空 source，或数值解析改 `from_chars` 返回 nullopt。
- **跨线程状态写竞争：epoch 失配分支在池线程直接写 `RasterOverlayTile::state_`** — [RasterOverlayTileProvider.cpp:3211](../../scaffold/src/earth_engine/providers/RasterOverlayTileProvider.cpp#L3211)。加载在途时主线程改 coverage/纹理尺寸触发 `invalidateSourceAssetDepotCache(++epoch)`，池线程 compose 完成走失配分支在池线程写 tile 态，与主线程 `getState` 读并发。窗口窄但非零。**修复**：失配/abandon 分支也推一条"失败"pendingUpload 由主线程终态化，保持"tile 态只在主线程写"不变量。
- **字节记账 bug：`SharedAssetDepot::put` 对已存在 key 覆盖时只加不减 `cacheBytes_`** — [SharedAssetDepot.h:64](../../scaffold/src/earth_engine/core/cache/SharedAssetDepot.h#L64)。重复 put 同一 key（刷新/重试完成）使字节计数无界虚增 → 远低于实际占用就疯狂驱逐。潜伏（模板未实例化）。**修复**：existed 分支先 `cacheBytes_ -= assetSize(旧)` 再赋值 + `+= assetSize(新)`。
- **上传限流忽略预算：`drainGpuUploadQueue` 无视传入 `FrameResourceBudget`，固定 `maxUploadsPerFrame=4`** — [TilesetContentLifecycleCoordinator.h:212](../../scaffold/src/earth_engine/tiling/TilesetContentLifecycleCoordinator.h#L212)。批量瓦片到位帧固定放行 4 个，弱 GPU 上可能超帧预算卡顿，预算充足时又压低吞吐拖慢补图。**修复**：走 budget 车道时间片（与 `processPendingUploads` 一致），或删掉未用形参明确"计数限流"契约。
- **半成品死分支：`uploadToGpu` 非 terrain 纹理绑定 lambda 定义后从不调用** — [GltfRenderResourcePreparer.cpp:947](../../scaffold/src/earth_engine/tiling/GltfRenderResourcePreparer.cpp#L947)。当前 dead-branch，但一旦把带纹理的非 terrain glTF 接入 GpuUploadQueue，纹理创建成功但 binding 永远 nullptr → 材质退化纯色。**修复**：删死 lambda 明确"仅服务无纹理 terrain"，或补全非 terrain 绑定分支——二选一别留半成品。
- **内存放大：结构化元数据按顶点深拷贝 `std::map`** — [GltfModel.cpp:8014](../../scaffold/src/earth_engine/content/GltfModel.cpp#L8014)。带 `EXT_mesh_features + EXT_structural_metadata` 的 primitive，几十行 feature 属性被复制成 N×顶点份 map（6.5 万顶点 → 6.5 万份 map + 字符串堆分配）。**修复**：`featureProperties` 按 featureId 索引存一份，顶点侧只留 featureIds 间接查。
- **重复累积：TilesetJson 外部 tileset 记录始终 Uri 类型，每次重请求重解析 + 递增 ordinal 生成重复子瓦片** — [GltfContentProvider.cpp:4078](../../scaffold/src/earth_engine/content/GltfContentProvider.cpp#L4078)。含外部子 tileset 时，卸载→重选→重请求每次跑一遍 `parseTilesetJson`，`records_` 累积重复子树，内存单调上涨。**修复**：解析成功后源记录改 External，以 `resolvedContentUrl` 为幂等去重键。

### P3（确认，低风险）

- HttpCache `size()`/`bodyBytes()` 无锁读共享 `map_/currentBodyBytes_`，与并发 put/get 数据竞争（[HttpCache.h:171](../../scaffold/src/earth_engine/core/cache/HttpCache.h#L171)）
- `SharedAssetDepot::invalidateAll` 清空 `inFlight_` 但不通知 waiters，回调永久丢弃 → 悬挂请求（[SharedAssetDepot.h:91](../../scaffold/src/earth_engine/core/cache/SharedAssetDepot.h#L91)）
- `AtmosphereParameters::validate()` 未守护 `ozoneDensityWidth`，为 0 时 SkyGradient 除零产 NaN 传入 clear color（[AtmosphereParameters.h:77](../../scaffold/src/earth_engine/environment/AtmosphereParameters.h#L77)）
- `drainGpuUploadQueue` 对已卸载瓦片调 `ensureTile` 会重建空 Unloaded 瓦片再丢弃（[TilesetContentLifecycleCoordinator.h:228](../../scaffold/src/earth_engine/tiling/TilesetContentLifecycleCoordinator.h#L228)）
- ancestor 替换后 `mappedSourceTiles_/directRasterTile_` 未更新，仍描述原 loading 映射（[RasterMappedToTilesetTile.cpp:407](../../scaffold/src/earth_engine/tiling/RasterMappedToTilesetTile.cpp#L407)）
- QM extId==4 metadata 截断时诊断把声明 JSON 长度硬报为 0，掩盖真实截断位置（[QuantizedMeshParser.cpp:167](../../scaffold/src/earth_engine/terrain/QuantizedMeshParser.cpp#L167)）
- availability 矩形解析不校验 `endX>=startX/endY>=startY`，倒置区间产 east<west 病态 Rectangle（[QuantizedMeshParser.cpp:65](../../scaffold/src/earth_engine/terrain/QuantizedMeshParser.cpp#L65)，降级 P3）
- oct-encoded 法线扩展按 `vc*2` 而非声明 `extLen` 读，extLen 小报时读入扩展体外尾随字节当法线（[QuantizedMeshParser.cpp:387](../../scaffold/src/earth_engine/terrain/QuantizedMeshParser.cpp#L387)，降级 P3；审核推翻了"级联误解析"次生结论，无偏移错乱）
- `loadTextures` 按 texture 而非 image source 解码，共享 source 的纹理重复解码各存一份全图（[GltfModel.cpp:2404](../../scaffold/src/earth_engine/content/GltfModel.cpp#L2404)，降级 P3，实为内存放大非结果错误）

---

## 2. 性能

### P1

- **每帧每瓦片每 primitive 全量深拷贝 `RenderCommand`** — [GltfDrawCommandBuilder.cpp:371](../../scaffold/src/earth_engine/tiling/GltfDrawCommandBuilder.cpp#L371)。`commands.push_back(cachedCommand)` 拷贝重结构（3 个 string + `vector<Texture*>` resize 15 + 定长块）。虽 uniforms 已句柄化为空 map（零分配），但 `stableKey`（非 SSO 长串）+ textures vector 每次拷贝仍各触发堆分配。数百瓦片 × 每 primitive ≥2 次分配 × 60fps = 每秒数万次 malloc/free，把常驻缓存省下的成本吐回。**修复**：帧列表侧改 emplace 轻量 draw handle（引用常驻命令 + 每帧覆盖字段）；textures 收敛为定长 `array<Texture*,N>`；stableKey 在常驻命令上持有，帧命令只存 `string_view`/索引，仅 clip 路径才复制串。
- **上采样白拷整份父地形顶点** — [GltfTerrainUpsampler.cpp:921](../../scaffold/src/earth_engine/content/GltfTerrainUpsampler.cpp#L921)。`make_unique<GltfModel>(parentModel)` 深拷贝整个父模型后第 922 行 `primitives.clear()` 又丢弃最重的 primitives。QM 地形 primitive ~4225 顶点，每瓦片白拷 vertices(~440KB)+baseVertices(~440KB)+terrainGpuVertexBytes(~135KB) ≈ 1MB 并立即释放。深缩放一屏物化数十子瓦片 → 掉帧尖峰。**修复**：构造空 GltfModel 只显式拷 textures/nodes/sceneRootNodes/preferredLocalOriginEcef 等元数据（`cloneMetadataOnly`），primitives 不拷，rasterOverlayDetails 无需拷（调用方随后 move 覆盖）。
- **SceneRenderPipeline 每帧对 1664 字节命令列表做 7-8 遍全量遍历** — [SceneRenderPipeline.cpp:101](../../scaffold/src/earth_engine/scene/SceneRenderPipeline.cpp#L101)。`sizeof(RenderCommand)=1664`（GltfUniformBlock 单独 880），大 stride 使每遍 cache 极不友好；其中 3-4 遍是纯诊断统计（imagery zoom / generation / terrain 计数）+ trace，均每帧无条件跑与消费者无关。**修复**：三个统计合并进 validate/applyMvpUniforms 同一遍；诊断+trace 加消费门控；把冷字段（surface*、overlay 数组）移到按需分配子结构缩小热 stride。

### P2（确认 + 降级自 P1）

- OBB 每帧每瓦片重建（Region 瓦片，含牛顿迭代测地转换），无缓存（[TileBoundsMetrics.cpp:284](../../scaffold/src/earth_engine/tiling/TileBoundsMetrics.cpp#L284)，降自 P1）
- `sampleTileBounds`/`sampleChildBounds` 对每 view 重复做两次 `tileIntersectsFrustum`（含 OBB 重建翻倍，[TileSelectionVisibilitySampler.cpp:48](../../scaffold/src/earth_engine/tiling/TileSelectionVisibilitySampler.cpp#L48)）
- 遍历执行器每瓦片重复 `cartesianToCartographic(相机)`，相机一帧内恒定（[TileSelectionTraversalExecutor.cpp:42](../../scaffold/src/earth_engine/tiling/TileSelectionTraversalExecutor.cpp#L42)，降自 P1）
- `summarizeForViews` 每瓦片每帧堆分配 `distances` 向量（单 view 为 1 元素堆块，[TileSelectionInputMetrics.cpp:64](../../scaffold/src/earth_engine/tiling/TileSelectionInputMetrics.cpp#L64)）
- `terrainChildren` 按值返回向量并逐一构造带 string 成员的 TileKey，frontier 瓦片每帧命中（[TileQuadtreeChildKeys.h:14](../../scaffold/src/earth_engine/tiling/TileQuadtreeChildKeys.h#L14)）
- `ensureProjectionDetailsFromModelBounds` 对全部顶点做两遍 `tryCartesianToCartographic`（[TileRasterOverlayDetailsGenerator.cpp:380](../../scaffold/src/earth_engine/tiling/TileRasterOverlayDetailsGenerator.cpp#L380)）
- 源瓦片 LRU 队列缓存命中路径无限增长（`cacheLru` 只追加不去重/裁剪，[RasterOverlayTileProvider.cpp:1992](../../scaffold/src/earth_engine/providers/RasterOverlayTileProvider.cpp#L1992)）
- `availabilityState` 每次遍历全部 layer + 递归四叉树下降 + 两次 32 位 morton 循环（[QuantizedMeshTerrainProvider.cpp:1502](../../scaffold/src/earth_engine/providers/QuantizedMeshTerrainProvider.cpp#L1502)）
- `WebMercatorProjection::geodeticLatitudeToMercatorAngle` 每次重算 `maximumLatitude()`（两次 atan+exp），逐顶点命中（[WebMercatorProjection.cpp:75](../../scaffold/src/earth_engine/core/geodesy/WebMercatorProjection.cpp#L75)）
- `Camera::viewMatrix()/projectionMatrix()` 每次调用重算（lookAt/tan），每帧多处独立调用无记忆化（[Camera.cpp:56](../../scaffold/src/earth_engine/scene/Camera.cpp#L56)）
- `VectorLayer::buildRenderCommands` 每帧为每 feature 重新 `createBuffer`，tessellation 已缓存但 GPU 上传逐帧重复（[VectorLayer.cpp:419](../../scaffold/src/earth_engine/layers/VectorLayer.cpp#L419)）
- 缓存卸载主循环每迭代两次全队列 O(N) 扫描 → O(N²)（[TileCacheUnloadCoordinator.h:82](../../scaffold/src/earth_engine/tiling/TileCacheUnloadCoordinator.h#L82)，降自 P1，已被时间片封顶）
- `processingOrder` 每渲染瓦片每帧堆分配 `vector<size_t>` + `stable_sort`（[TileRenderCommandPreparer.h:56](../../scaffold/src/earth_engine/tiling/TileRenderCommandPreparer.h#L56)，在册 P2-21 具体病灶点，降自 P1）
- 上采样瓦片创建无条件 `platformLog(Info)` 主线程格式化+写（[TileGltfTerrainUpsampledChildMaterializer.h:49](../../scaffold/src/earth_engine/tiling/TileGltfTerrainUpsampledChildMaterializer.h#L49)，降自 P1）
- 每帧无条件深拷贝重建 `PresentationTrace`，O(N) 次字符串堆分配无消费门控（[ScenePresentationTraceBuilder.cpp:212](../../scaffold/src/earth_engine/scene/ScenePresentationTraceBuilder.cpp#L212)，降自 P1）

### P3（确认）

- `applyPerFrameCommandState` 每帧每命令固定 4 次拷贝 overlay uniform 数组含空槽（[GltfDrawCommandBuilder.cpp:337](../../scaffold/src/earth_engine/tiling/GltfDrawCommandBuilder.cpp#L337)）
- `computeRuntimeJointMatrices` 共享 skin 的多 primitive 每帧各算一遍关节矩阵（[GltfModel.cpp:8409](../../scaffold/src/earth_engine/content/GltfModel.cpp#L8409)）
- `EllipsoidTerrainContentProvider::requestTileContent` 调用线程同步建整块椭球网格（逐顶点 cartesianToCartographic，[EllipsoidTerrainContentProvider.cpp:364](../../scaffold/src/earth_engine/content/EllipsoidTerrainContentProvider.cpp#L364)）
- `processPendingUploads` 每瓦片无条件 Info 日志打在主线程热上传路径（[RasterOverlayTileProvider.cpp:3610](../../scaffold/src/earth_engine/providers/RasterOverlayTileProvider.cpp#L3610)）
- `mortonEncode2D` 用 32 次逐位循环替代 O(1) 魔数位交织，availability 热路径每次两遍（[QuantizedMeshTerrainProvider.cpp:625](../../scaffold/src/earth_engine/providers/QuantizedMeshTerrainProvider.cpp#L625)）
- `OrientedBoundingBox` 构造无条件 `glm::inverse(dmat3)`，`inverseHalfAxes_` 仅冷路径 contains() 用、accessor 零调用（[OrientedBoundingBox.h:35](../../scaffold/src/earth_engine/core/math/OrientedBoundingBox.h#L35)）
- MVP 更新热循环每命令做 `cmd.owner=="globe"` 死字符串比较（全工程无 owner 为 globe，[SceneRenderCommandUniformUpdater.cpp:31](../../scaffold/src/earth_engine/scene/SceneRenderCommandUniformUpdater.cpp#L31)）
- `Engine::render` 每帧无条件 `snprintf` 构建 256 字节 detail 串，即使 logTiming 本帧不输出（[Engine.cpp:108](../../scaffold/src/earth_engine/Engine.cpp#L108)）
- `PickingService` 每次拾取把矢量 feature 全量顶点重复转 ECEF，与 VectorLayer 已缓存 tessellation 重复（[PickingService.cpp:157](../../scaffold/src/earth_engine/interaction/PickingService.cpp#L157)）
- `sampleGltfTerrainHeight` 非实例情形也堆分配 1 元素 `vector<Mat4>`（[LoadedTerrainHeightSampler.cpp:168](../../scaffold/src/earth_engine/tiling/LoadedTerrainHeightSampler.cpp#L168)，降自 P2，被同函数大分配盖过）

---

## 3. 优雅度 / 技术债

### 死代码（约 1400+ 行，纯删/纯移，低风险高收益）

| 死代码 | 位置 | 处置 |
|---|---|---|
| `GltfRenderResourcePreparer::prepareCpuWork/prepareCpuWorkFromModel`（~230 行，无调用点） | [GltfRenderResourcePreparer.cpp:646](../../scaffold/src/earth_engine/tiling/GltfRenderResourcePreparer.cpp#L646) | 异步 glTF 预备管线短期不落地则删，连带精简仅它们用的 GpuReadyData 分支 |
| `Uri` 类（Uri.h/.cpp ~340 行，仅单测用；3 provider 各自平行复刻 URL 工具） | [Uri.cpp:110](../../scaffold/src/earth_engine/core/net/Uri.cpp#L110) | 二选一：3 provider 委托 Uri，或删 Uri + test_uri。不能两存 |
| `PersistentCache` 全家 + HttpCache 磁盘持久化（`setCacheDir` 从不被调，`cacheDir_` 恒空，persist/load 永远短路） | [PersistentCache.h:18](../../scaffold/src/earth_engine/core/cache/PersistentCache.h#L18) | 二选一：初始化处接线并补 ensureDir，或删 PersistentCache + HttpCache persistAsync + 头注释承诺 |
| `Future<T>.then()` 声称链式但未实现，`AsyncSystem::run` 返回值处处丢弃 | [AsyncSystem.h:100](../../scaffold/src/earth_engine/core/async/AsyncSystem.h#L100) | 删 Future 包装 run 直接返回 void，或真正实现 then()；至少先删头注释虚假承诺 |
| `TileTextureCache`（.cpp+.h ~173 行，仅 CMakeLists 引用） | [TileTextureCache.cpp:8](../../scaffold/src/earth_engine/renderer/TileTextureCache.cpp#L8) | 删模块 + CMake 条目 |
| core/geodesy 测地线簇：Vincenty inverse/direct、EllipsoidTangentPlane、SimplePlanarEllipsoidCurve（无生产调用） | [Ellipsoid.cpp:262](../../scaffold/src/earth_engine/core/geodesy/Ellipsoid.cpp#L262) | 近期无需则删，保留则头注"scaffold 未接线"+补集成用例锁契约 |
| `kTileVertexMSL/kTileFragmentMSL` 死 MSL shader（~90 行） | [Renderer.cpp:1126](../../scaffold/src/earth_engine/renderer/Renderer.cpp#L1126) | 删或纳入 `#if 0` 参考块 |
| `readyTexture_` 只写不读的死状态字段 | [RasterMappedToTilesetTile.h:182](../../scaffold/src/earth_engine/tiling/RasterMappedToTilesetTile.h#L182) | 删字段 + 5 处赋值，texture() 保持读 `_pReadyTile->getTexture()` |
| `activeWorkerBlockingRequests_/peakWorkerBlockingRequests_` 从不自增，诊断恒报 0（误导） | [QuantizedMeshTerrainProvider.cpp:2312](../../scaffold/src/earth_engine/providers/QuantizedMeshTerrainProvider.cpp#L2312) | 异步化后无阻塞语义则删这对成员+诊断字段，否则补自增点 |

### 上帝文件（整体重构，纯移动过 140 测试即安全）

- **GltfModel.cpp 8981 行**：约 8500 行挤在单一匿名 namespace（[:25](../../scaffold/src/earth_engine/content/GltfModel.cpp#L25)）；`parsePrimitive` 单函数 992 行含 15 段逐字复制的纹理绑定样板（[:7074](../../scaffold/src/earth_engine/content/GltfModel.cpp#L7074)）。**拆**：建 `GltfModelInternal.h` 承载跨域契约类型，按职责拆 7 个 TU（BinaryAccess/SchemaValidation/MaterialParse/Metadata/AnimationRuntime/Instancing/GeometryAssembly）纯机械搬移；`parsePrimitive` 抽 `parseOptionalTextureBinding` 把 15 段收敛成 15 行调用。
- **GltfContentProvider.cpp 4267 行**：URL 解析 / 四种容器解码 / tileset.json 校验 / 异步编排 / 两个 provider 五类职责混杂（[:1](../../scaffold/src/earth_engine/content/GltfContentProvider.cpp#L1)）。**拆**：UrlResolver / TileContentContainerDecoders / TilesetJsonValidation / ContentRequestPipeline 四个 TU，容器解码器抽出后可对畸形字节做针对性单测。
- **RasterOverlayTileProvider.cpp 3724 行**：合成算法 / 深度盒 / depot / 异步编排 / 像素 blit 全混（[:1043](../../scaffold/src/earth_engine/providers/RasterOverlayTileProvider.cpp#L1043)）。**拆**：RasterImageCompositor（纯像素可独立单测）/ RasterSourceCoverage / RasterSourceAssetDepot。
- **Renderer.cpp 2622 行**：86%（2250 行）是内嵌 GLSL/MSL 字符串，真实 C++ 仅约 370 行（[:36](../../scaffold/src/earth_engine/renderer/Renderer.cpp#L36)）。**拆**：shader 源外移到 `ShaderSources.cpp/.h`（每组 GLSL+MSL 相邻），Renderer.cpp 只留 Impl+builder+初始化。

### 分层 / 头文件卫生

- **core→tiling 反向依赖**：`S2CellID.cpp` 仅为一个薄便利重载 include `TileKey.h`，最底层却依赖上层（[S2CellID.cpp:4](../../scaffold/src/earth_engine/core/geodesy/S2CellID.cpp#L4)）。**修**：删重载 + include，调用方传三整型；确需便利入口则移到 tiling 层 helper。
- **6+ 组双向 include 环**：scene/renderer/providers/tiling/layers/camera 之间，根因是 `Camera/FrameState` 等基础类型错放 scene/ 层（[Camera.h:1](../../scaffold/src/earth_engine/scene/Camera.h#L1)）。**修**：新建低层 `view/`（或 core/view/）容纳 Camera/Frustum/FrameState，各层向下引用，分组消环每组过全量编译+140 测试。
- **include 路径三套并存**：`earth_engine/` 前缀 16 处 / `../` 相对 531 处 / 裸相对 931 处（[S2CellID.cpp:4](../../scaffold/src/earth_engine/core/geodesy/S2CellID.cpp#L4)）。**修**：选定顶层前缀约定脚本批量统一 + CI 守护。
- `RenderDeviceRasterTextureUploader.cpp` 用 9 处 `std::memcpy` 未 include `<cstring>`，依赖传递包含（[:37](../../scaffold/src/earth_engine/renderer/RenderDeviceRasterTextureUploader.cpp#L37)）
- `jsonUint32OrDefault` 在 QM parser 与 provider 各复制一份；URL 工具族 provider 内自成一套（[QuantizedMeshTerrainProvider.cpp:963](../../scaffold/src/earth_engine/providers/QuantizedMeshTerrainProvider.cpp#L963)）
- `Engine::render` 主渲染热路径残留 `fprintf(stderr)` 调试打印（[Engine.cpp:51](../../scaffold/src/earth_engine/Engine.cpp#L51)，Scene.cpp:61 同类）
- `GltfRenderResourcePreparer::prepare` 的 primitive→resources 抄写与 prepareCpuWork 逐字段重复，单文件内三份材质拷贝（[:310](../../scaffold/src/earth_engine/tiling/GltfRenderResourcePreparer.cpp#L310)）
- `boundsVisible`/`sampleTileBounds` 可见性判断逻辑三处复制（[TileSelectionVisibilitySampler.cpp:25](../../scaffold/src/earth_engine/tiling/TileSelectionVisibilitySampler.cpp#L25)）
- tiling/ 268 文件全平铺零子目录（[Tileset.h:1](../../scaffold/src/earth_engine/tiling/Tileset.h#L1)，降 P3——审核指出平铺是本工程主流风格非异类，真缺陷是体量导致的导航/审计成本；Selection/Traversal/Kick/Refine 家族实测 66 文件可先归一子目录）
- telemetry 类型碎片化：16 个 `*Diagnostics*` 散落 providers/tiling/scene 三层（[SceneTilesetDiagnostics.h:11](../../scaffold/src/earth_engine/scene/SceneTilesetDiagnostics.h#L11)，降 P3，审核修正部分机理）

---

## 4. 已证伪（防止未来重查）

| 原报（审计员） | 核实结论 |
|---|---|
| `TileLoadRequestDispatcher` complete 续体按引用捕获 lifecycle 锁/状态，生命周期安全依赖析构等待不被绕过（[TileLoadRequestDispatcher.h:95](../../scaffold/src/earth_engine/tiling/TileLoadRequestDispatcher.h#L95)） | **误报**。析构等待契约成立，捕获安全 |
| upsampler 边缘检测 `equalsEpsilon(1e-5)` 把接近 0/1 内部顶点误判裙边（[GltfTerrainUpsampler.cpp:408](../../scaffold/src/earth_engine/content/GltfTerrainUpsampler.cpp#L408)） | **误报** |
| decoder 返回像素未按 `width×height×channels` 校验致 GPU 上传越界（[GltfModel.cpp:2406](../../scaffold/src/earth_engine/content/GltfModel.cpp#L2406)） | **误报**，有校验 |
| `TilesetJsonContentProvider` 构造函数同步阻塞 httpGet 最长 20s（[GltfContentProvider.cpp:3710](../../scaffold/src/earth_engine/content/GltfContentProvider.cpp#L3710)） | **误报/有意**，跑在 installScene 主线程同步初始化契约（与 P2-2 澄清一致） |
| 内容请求回调裸捕获 this，provider 析构后迟到 completion 触碰已析构成员（[QuantizedMeshTerrainProvider.cpp:1750](../../scaffold/src/earth_engine/providers/QuantizedMeshTerrainProvider.cpp#L1750)） | **误报**，有 metadataRegistry 生命周期守卫 |
| `S2CellBoundingVolume` 构造硬编码 WGS84 忽略参数化椭球（[S2CellBoundingVolume.cpp:193](../../scaffold/src/earth_engine/core/geodesy/S2CellBoundingVolume.cpp#L193)） | **误报**，当前无非 WGS84 场景 |
| `installScene` 重装先析构 raster overlay，旧 Tileset 悬垂指针在阻塞 HTTP 期被 worker 访问 use-after-free（[EarthEngineSdkFacade.cpp:182](../../scaffold/src/earth_engine/sdk/EarthEngineSdkFacade.cpp#L182)） | **误报**，析构时序安全 |

**duplicate-known**：`refreshRenderEntries` 每帧新建 3 个 `unordered_set<string>` 对每瓦片拼接去重串（[TileRenderPlanFinalizer.h:42](../../scaffold/src/earth_engine/tiling/TileRenderPlanFinalizer.h#L42)）= 在册 P2-10 同型模式的另一位点。

---

## 5. 建议动作（按 ROI 排序）

1. **两条真实可复现正确性（各 <1h，手术式，立即可做）**：SkyBox/AtmosphereBackgroundPass `.release()` 泄漏 → 改 unique_ptr + dispose reset；GoogleMapTiles 可用性向量 → 加 `availabilityMutex_`。
2. **三处递归深度上限（防对抗性输入崩溃，各 <1h）**：glTF 节点树、tileset.json parseTile、（顺带）结构化元数据按顶点 map 放大。前两个加深度常量或改迭代遍历。
3. **性能三连（约半天-1 天）**：RenderCommand 帧列表改轻量 handle（消每帧每 primitive 拷贝）；SceneRenderPipeline 诊断遍历合并+门控；upsampler `cloneMetadataOnly`（消 ~1MB/瓦片白拷）。
4. **core/cache 两个潜伏 bug（接线前修，各 <30min）**：SharedAssetDepot deque→list、put 字节记账先减后加。
5. **死代码清理（约半天，纯删/纯移，过 140 测试即安全）**：先删无争议的（TileTextureCache、死 MSL、readyTexture_、死诊断计数、prepareCpuWork），再对 Uri/PersistentCache/Future/geodesy 簇做"接线 or 删除"二选一决策。
6. **上帝文件拆分 + 分层消环（整体重构，单列任务）**：GltfModel 7-TU 拆分、Renderer shader 外移、Camera/FrameState 下沉 view/ 消环。纯机械搬移优先，行为零变更。
