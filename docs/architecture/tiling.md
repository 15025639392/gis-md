# 架构沉淀:瓦片调度子系统 (tiling)

> **架构/方案**文档:怎么搭、为什么、接新功能从哪切。质量标尺看 `docs/northstar/*`;行号看 `AI_INDEX.md` §5/§6/§20;历史事故看 `docs/issues/tile-render-pipeline-gap-audit-2026-07-06.md`(下称"gap-audit")。行号随重构漂移,以符号名为准。

**规模**:`tiling/` 268 文件、34k 行,**引擎重心**。选择/遍历/加载/缓存逐算法对齐 cesium-native。

---

## 职责边界

**管**:
- 四叉/八叉瓦片树每帧选择/遍历(剔除、SSE 细化、kick、遮挡)→ 产出 `TilePlan`。
- 瓦片内容生命周期状态机(Unloaded→ContentLoading→ContentLoaded→Done/Failed)及跨帧持久化。
- 异步加载请求调度、优先级排序、帧级预算门控(`TileLoadScheduler`、`core/resources/FrameResourceBudget`)。
- CPU→GPU 上传两阶段流水线(`GltfRenderResourcePreparer::prepareCpuWork`/`uploadToGpu`、`GpuUploadQueue`)。
- 字节预算 LRU 淘汰(`TileContentCacheManager`,对齐 cesium `Tileset::_unloadCachedTiles`)。
- 栅格叠加映射与 upsample 协调(详见 `imagery.md`)。
- 渲染前可渲染性判定与命令生成入口(`TilesetRenderFrameExecutor`),但**不**提交 GPU draw。

**不管**:GPU 资源创建/绘制执行(→`renderer/`,经 `IPrepareRendererResources` 解耦)、glTF 顶点格式构建算法(→`content/GltfRenderGeometryBuilder`,tiling 只驱动时机)、HTTP 传输本身(→`platform/` curl 桥,tiling 只持 `CancellationToken`)、3D Tiles 内容解码(→`content/GltfContentProvider`)。

**边界模糊点**:页存储合成(`TerrainPageLayerPool`/`TerrainPageStore`)物理上在 `renderer/`,但经 `WorkLedger`/`TileScheme`/`TileKey`/`RasterOverlayProjection` 与 tiling 强耦合。README 记它在 tiling 职责,实现却不在此目录(是否计划迁移未验证)。

---

## 核心设计决策 + 理由

### 1. 极端小类分解(268 文件 vs 几个大类)★
root 选择、细化、kick、剔除、复用、plan-append、遍历上下文构建各自独立成类,策略类多为 header-only、内联、零 vtable。
- **理由**:选择/遍历是 cesium-native 移植,细粒度分解保持算法保真的同时让每策略可独立单测。gap-audit 独立审计判"净正向":seam 有真实单测 + `test_selector_cesium_golden_diff.cpp` 逐帧字节对拍;policy 零间接税(`submit()` 是唯一每帧虚函数)。
- **代价(纯工程)**:目录扁平无子目录,单一逻辑流散在约 8 跳,导航/审计税高;且与同库 4 个 god-file(`GltfContentProvider.cpp` 4267 行)并存,呈"既过度又不足分解"的不一致。

### 2. cesium-native 算法对齐取向
选择/遍历/加载/缓存逐算法对齐 cesium-native,而非自研或对齐其他引擎。**对照实现的测试是行为规格的一部分**——移植时从 cesium `test/Test*.cpp` 提取输入/期望/边界/容差。gap-audit 结论:"算法保真(选择/流式/地形/影像映射)= 生产级"。

### 3. 选择遍历如何产出 TilePlan
遍历是**纯 C 函数指针"vtable"**(`TileSelectionTraversalContext`)驱动的递归下降(`visitTileIfNeeded`→`visitTile`),按 SSE/剔除/遮挡决定细化,fan-out 后 kick/preload 收尾,写入 `TilePlan`(`visibleTiles`/`renderEntries`/`selectionRecords`)供渲染直接消费、不重选 LOD。
- `TilePlan` = cesium `ViewUpdateResult` 等价物。
- 函数指针而非虚函数是**故意的类型不安全**权衡(源码注 "TYPE-UNSAFE"):两个不同 `void*` userData 混用会静默重解释指针——已知脆弱点,不是疏忽。

### 4. 异步在途的单一账本 WorkLedger ★
`core/async/WorkLedger` 用 RAII `Ticket` 区分两类在途:**Landing**(别的线程会自己走完,持有期不需出帧,释放时才需一帧消费)与 **Pumped**(必须在渲染帧里推进,持有期必须持续出帧)。
- **理由**:此前"还有活没干完吗"由 `Scene::hasConvergingWork` 在外部**重新推导**——分别问 tileset/overlay/pageStore/相机四个各自为政的判据,其中两个已因此出过 bug(页在途判据无终止态、pan 惯性永不归零)。令牌化把失效方向从"画面冻住且零报错"反转为"忘取令牌仍冻屏(不更差),取了忘释放则永不入睡且 awake reason 报出 label"——危险方向从默认变成需故意为之。
- **关键教训**:此前把 Landing/Pumped 一视同仁当 Pumped,一个在飞网络请求就能把满帧率按住其整个生命周期。

### 5. GPU 上传队列 GpuUploadQueue
`std::mutex` 守护的 `std::deque<PendingGpuUpload>` 严格 FIFO,`push`/`tryPop` 都在**主线程同一帧内**(不是 worker→main 跨线程队列)。
- **理由**:拆分唯一收益是 drain 侧每帧上传上限(`maxUploadsPerFrame`=4)带跨帧溢出,而非并发本身;worker 只做更早的顶点字节转换。
- **短板**:队列是 FIFO 非优先级序,doc-comment 声称"pop 最高优先"是假的(gap-audit P2);高 SSE 瓦片不能优先上屏。
- 该 32B `TerrainGpuVertex` 异步路径整体**功能完整但配置默认关闭**——门需要 `hasTerrainWaterMaskMetadata`+完整 `terrainGpuVertexBytes`,而 `decoupleImageryFromGeometry=true`(demo 默认)禁用其唯一生产者。

### 6. byte-budget LRU 淘汰
`TileContentCacheManager` 持 `totalBytesUsed_`+`TileUnloadQueue`,按估算字节对 `maximumCachedBytes`(默认 512 MiB)做 LRU;交互期超预算**延后**非 Unloading 瓦片避免单帧尖峰。对齐 cesium `_unloadCachedTiles`。
- 相邻短板:`SharedAssetDepot` 字节预算用浅 `sizeof` 低估真实资产,逐出几乎不触发(gap-audit P2;是否影响 tiling 自身 `TileCacheMetrics` 独立口径未验证)。

### 7. SSE 细化判据
`screenSpaceErrorForView` 投影中心点与偏移 `geometricError` 的点,取 NDC y 差值乘视口高度半值,是 cesium `computeScreenSpaceError` 的 NDC 投影形式;`geometricError≤0` 短路,距离下限钳 1e-7。对齐是为便于逐帧 golden 字节对拍。

---

## 数据流(`Tileset::update()` 一帧)

委托给 `TilesetUpdateFrameFacade::update`,实际驱动者 `TilesetUpdateFrameRuntime::run`:

```
1. ++generation_                     // 渲染命令校验的输入
2. TileFrameWorkCoordinator::run
   ├─ 复用分支: 视角几乎不变(位置<1e-3m、方向 lenSq<1e-12)→ 跳过完整遍历,只刷渲染条目
   └─ 非复用: TileSelectionFrameFacade::selectTiles
       → 根 fan-out(TileSelectionRootPolicy)
       → 每根 TileSelectionTraversalExecutor::visitTileIfNeeded 递归下降:
           a. 视锥/雾剔除 + SSE(TileSelectionVisitPreparation)
           b. visitTile: 栅格叠加准备 → 剔除/遮挡/细化决策 → 叶子/SSE 满足则 finishAsSingleTile 入 plan
              否则 ensureTileChildren 递归子节点 → post-traversal: kick/祖先预加载/plan 提交
   ├─ 预取: TileRasterOverlayFrameProcessor::prefetchSelection(仅非复用帧)
   └─ 请求发出: requestMissingTiles → TileLoadScheduler(排序 + FrameResourceBudget 门控在飞数)
                → TileLoadRequestDispatcher::requestContent(异步) → 回调推入 TilePendingLoadQueue
3. processPendingLoads: 先 terminal 结果, 再 upload 结果
   → 满足异步地形 32B 条件者 push 进 GpuUploadQueue, 并保留上传声明(asyncGpuUploadPending=true)
4. drainGpuUploadQueue (在 3 之后同帧, 契约 LoadsBeforeGpuDrain 保证顺序)
   → tryPop 最多 maxUploadsPerFrame(4) → uploadToGpu 建 buffer → eraseUpload 释放保护
5. buildRenderCommands: 遍历 tilePlan_, 只消费已选瓦片, 不重选 LOD
6. unloadCachedBytes: collectInactiveTiles + TileUnloadPolicy → 交互期延后驱逐, 否则 TileCacheUnloadCoordinator
```

---

## 关键契约与不变量

| 契约 | 说明 / 出处 |
|---|---|
| `processPendingLoads` 必须先于 `drainGpuUploadQueue` | 前者是后者生产者;契约 `LoadsBeforeGpuDrain`。反转不报错,只让上传延迟一帧,易误诊为网络/设备问题 |
| `renderer.submit` 必须先于 `releaseRenderReferences` | 命令持裸 `Buffer*/Texture*` + `resourceKeepAlive`;提前释放=绘制中途释放 GPU 资源 |
| GpuUploadQueue 严格 FIFO | 契约 `GpuUploadQueueFifo` 双序列计数器机检,重排/swap 立即触发 |
| 在途异步上传不可卸载(在途不可淘汰) | `asyncGpuUploadPending`+保留 upload key 是唯一保护;必须**恰一处**释放,否则永久钉住。`ContentLoading` 态一律 Keep。**未机检** |
| Failed 态必须进保护集 | 否则 fill 代理每帧重建成风暴(掠视 40ms) |
| CancellationToken 桥接 | 每在飞请求存 token,tileset 析构时统一取消并等待 |
| 两把独立互斥量不可混淆 | `TileLoadLifecycle::mutex_` 守 requestState+pendingLoads;`GpuUploadQueue` 有第二把独立锁,只防 unload 路径与 push/pop 竞争(push/pop 都在主线程) |
| 局部预算 + Scene 帧级总量双重门控 | `FrameResourceBudget` 保留每个 Tileset 的 lane/inflight/时间保护，同时接入 `SceneFrameResourceArbiter`；多个 Tileset 与 raster/MVT/PageStore 共享 Scene stage 总量。Scene 层识别 Urgent/Normal/Preload，并为 terrain urgent 保底；局部队列仍负责更细的组内顺序 |
| MVT source 级公平 | MVT 多 source 在 Scene 层按剩余 grant 分片并轮转起始 source；实例份额耗尽不会伪造全局 denial，未用额度可回流。Terrain 多 Tileset 尚无实例级轮转，仍按 primary→pending→content 消费共享 grant |
| two-phase FrameState | 命令带 `frameId`/`generation`,`validateMvpRenderCommands` 拒绝 frameId 不匹配或 generation==0 |
| WorkLedger label 必须静态字面量 | 脏位只存指针,label 要活到诊断打印那一刻 |

---

## 诚实得失

### ✅ 强项
- 选择/遍历/SSE/kick/遮挡逐行对齐 cesium-native,有独立单测 + golden 逐帧字节对拍护航。
- 268 文件极端分解经独立审计判"净正向":单测粒度换算法保真,零运行时虚函数间接税。
- byte-budget LRU 卸载存在且接线正确,交互期避免单帧驱逐尖峰。
- `WorkLedger` Landing/Pumped 区分是对真实生产 bug(冻屏、pan 惯性不归零)的针对性修复,失效方向反转为默认较安全。
- 瞬时失败经 `FailedTemporarily` 重试不阻塞子树,可回落父级/椭球,glTF 解析边界检查到位。
- 调度基础设施(frame budget/优先级分组/preloadAncestors&Siblings/loadingDescendantLimit)独立调研判"已是生产级"。

### ⚠️ 短板 / 已知债
- **优先级仍有一处边界**:`GpuUploadQueue` 保持严格 FIFO，Scene arbiter 只决定
  terrain producer 本帧能上传多少，不在队列内部按 tile 紧急度重排；这是保持
  FIFO/生命周期确定性的取舍，不等价于 tile 级 urgent GPU 优先队列。
- **上传/finalize 默认预算极紧**(`maxMainThreadFinalizesPerFrame`=1、`maxRasterUploadsPerFrame`=1),是"下载完成→能画"延迟的观感天花板之一。
- **无退避重试**:`FailedTemporarily` 每帧重打服务器,自造 DoS(gap-audit P1)。
- **无请求级取消**(有 defer 无 cancel),快速平移仍等废请求跑完。
- **异步 32B 地形顶点上传路径完整但默认关闭**——"看起来是死代码实则是关掉的活代码"的典型陷阱。
- **每帧每 primitive 深拷贝 `RenderCommand`**(sizeof=1664);上采样每帧深拷父地形整个 `GltfModel` 再丢弃(gap-audit P1)。
- **反向分层与 include 环**:`core/geodesy/S2CellID.cpp` include `tiling/TileKey.h`,6+ 双向环。
- 承接自其它模块的债:**T-P5**(模板键碰撞元凶未定位,疑 `std::hash<SchemeId>` 哈希 interned 指针撞车)、**T-P3**(`ensureFillProxy` 无帧预算封顶)、**T-P6**(GPU shader host 无执行级守卫)。
- **未复核-可能过时**:gap-audit §8.1 列的 07-05 P1 correctness 项在 07-06 复审仍 OPEN(`SharedAssetDepot` 缓存 `deque::iterator` 潜在 UB、`GoogleMapTilesImageryProvider` availability 向量无锁跨线程读写 UAF 风险、glTF/tileset.json 递归无界深度)——日期已久,**未复核当前源码是否已修**。
- 隐式瓦片子系统(`OctreeTilingScheme`+`TileAvailability`,1792 实现+2466 测试)与生产加载管线**零交叉引用**,但**用户已裁决保留,勿提议删**。

---

## 扩展点

- **新 Provider 类型**:实现 `TerrainProvider`/`ImageryProvider` 接口,经 `TilesetTerrainProviders` + `TileLoadRequestPlanner::classify`(Skip/TerrainContentUpsample/Content 分支)接入,**不需改选择遍历核心**。
- **新剔除策略**:在 `TileSelectionCullingPolicy`(视锥/雾)或 `TileSoftwareOcclusionPolicy`(地平线)旁新增同构策略类,在 `TileSelectionVisitPreparation::prepare` 挂接。cesium 有 `ITileExcluder`(clipping polygon/region mask)本引擎**未实现**,可作扩展参考。
- **新调度信号**:局部 Tileset 工作先新增 `FrameResourceLane` 与 limit 映射；若会与
  raster/MVT/PageStore 竞争同类资源，还必须映射到
  `SceneFrameResourceProducer`+`SceneFrameResourceStage`，在 seal 前声明 demand，
  在真实副作用前 `tryAcquire`。涉及跨帧在途生命周期仍应经
  `WorkLedger::acquire(Kind,label)` 注册，而不是在 `Scene::hasConvergingWork`
  手写新判据。
- **新加载优先级维度**:`TileLoadPriorityPolicy` 当前 `{Preload,Normal,Urgent}`+组内序;扩展需同改 `hasHigherPriority`、`toFramePriority`、`TilePendingLoadQueue` 选取逻辑。
- **优先接线未完成的既有能力**(优于新写代码):异步地形顶点上传(仅需 `decoupleImageryFromGeometry=false` 或提供另一生产者)、`TileIncrementalFrontier`(已捕获未剪枝)、影像 provider 统一 HTTP 缓存接入(目前仅 QM/heightmap 走 `HttpCache`,XYZ/Bing/Google/TMS/WMS/WMTS 全直连)。

---

## 对照系

**对齐 cesium-native(具体算法)**:SSE 公式(NDC 投影形式)、`_visitTileIfNeeded`/`_visitTile` 递归下降、kick 机制、遮挡 children-union 聚合、`TraversalDetails` 冒泡合并、`_unloadCachedTiles` 字节淘汰、`TilesetContentManager` 三阶段(request/upload/drain)、`prepareInLoadThread`/`prepareInMainThread` 两段式(**结构**对齐;本引擎两段实际都跑主线程,非并发模型对齐)、`ViewUpdateResult`(TilePlan)、`UpsampledQuadtreeNode`。

**自研/非对照**:
- **帧间选择复用**(`TileSelectionReusePolicy`):视角不变跳过完整遍历,cesium 无直接等价。
- **`WorkLedger`**:账本化在途工作是自研,针对本引擎"四判据各自为政"问题。
- **`SceneFrameResourceArbiter`**:Scene 实例级两阶段资源仲裁是自研；cesium-native
  提供 Tileset 内容生命周期与请求调度行为参考，但没有本项目这种把多个 Tileset、
  MVT 与 PageStore 放进同一 producer×stage 帧配额表的组合根。
  demand 采用活跃 producer 启动额度 + 上一帧 used/denied 自适应 hint，而不是启用即
  固定占满预算；各执行点仍在真实副作用前 `tryAcquire`，拒绝工作留队跨帧续跑。
- **`viewerRequestVolume` 门控**在 visit 路径存在,cesium inline selection 无此机制——新增而非缺失,会使输出偏离 cesium。
- **`TerrainPageLayerPool`/`TerrainPageStore`**(等尺寸块 LRU 页表)是"北极星合成方案"自研设计,非 cesium 概念。
- **已删除**:异步选择/影子树(`TileSelectionShadowRunner` 等)2026-08-07 全删——release 下 selector 非瓶颈从未生产启用,增量切面等价性 oracle 裁决 NO-GO。
