# 架构沉淀:基础设施与场景装配 (core + scene)

> **架构/方案**文档,覆盖引擎的"地基与骨架"。行号看 `AI_INDEX.md` §1/§2/§3/§4/§12/§18;行号随重构漂移,以符号名为准。标"未验证"处为 AI_INDEX 既有断言、本文未逐行复核。

**规模**:`core/` 7.3k 行(数学/大地测量/异步/缓存/网络)+ `scene/` 7k 行(场景协调器)+ `sdk/`(facade)+ `Engine.h/.cpp`(帧循环)。

---

## 职责边界

- **core/**:平台无关地基层——数学(`Vec3`/`Mat4`/包围体/裁剪/相交,`core/math/`)、大地测量(椭球/投影/GCJ-02/S2,`core/geodesy/`)、异步原语(`AsyncSystem`/`WorkLedger`,`core/async/`)、缓存(`HttpCache`/`PersistentCache`,`core/cache/`)。**不知道"瓦片""场景""相机"**,只提供被上层复用的纯值类型与线程原语。
- **scene/**:场景装配与帧编排层。`Scene` 是"cesium-native 无直接对应物"的引擎自研组合根,owns Camera/CameraSystem/Renderer/渲染管线,把工作全部委派给 5 个 Coordinator + 1 个渲染管线,自己不做算法只做装配与阶段编排。**不实现**瓦片选择算法(在 `tiling/`)、具体渲染 API 调用(在 `renderer/`/`platform/`)。
- **sdk/**:`EarthEngineSdkFacade` 是面向调用方的一次性装配入口,把声明式 `EarthSceneConfig` 翻译成 provider/overlay/tileset 对象图,注入调用方已建好的 `Engine`。facade 不拥有 Engine/RenderDevice/PlatformBridge("Caller owns")；MVT source 由 facade 创建、由 `Scene` 以 source+`FeatureRenderLayer` runtime bundle 唯一托管。
- **Engine**:平台面向的生命周期外壳 + 输入路由,owns 唯一一个 `Scene`,几乎每个公开方法都是薄转发。

---

## 核心设计决策 + 理由

### 1. 大地测量以 cesium-native 为规格 + 对照测试即行为规格 ★
`core/geodesy/` 每个类型在 AI_INDEX 都标注对应的 cesium-native 类型;这不是巧合命名而是**移植**:算法步骤(Newton 迭代 `tryScaleToGeodeticSurface`、Vincenty 正反算、Web Mercator Gudermannian 公式)与 cesium 逐行对应,连容差常量命名都沿用(`kEpsilon12`=1e-12)。
- **对照测试即行为规格**:`tests/unit/geodesy/*` 中测试名直接写 `...MatchesCesiumNative`,注释写明"cesium 对应字段/语义是什么、这里是否一致"。改动大地测量代码时,这些测试名本身就是规格文档。
- **理由**:大地测量的坑集中在数值边界(极点、反经线、退化),cesium 已踩过一遍并稳定多年,重新发明成本远高于对照移植。`cesium-alignment-audit-2026-07-04` 独立审计印证:"core 数学/QM 解析/上采样/瓦片选择/scene 全部算法级对齐",真实差距不在算法而在异步契约。

### 2. WorkLedger:异步在途的单一账本 ★
存在理由:此前"还有活没干完吗"由 `Scene::hasConvergingWork` 在外部**重新推导**——同时问四个各自为政的判据(tileset/overlay/pageStore/相机),正确性=四者同时正确,漏一个的症状是**画面冻住且零报错**,其中两个已因此被修过。`WorkLedger` 把判据收敛到单一入口。
- **两种令牌语义相反**:`Landing`(别线程会自己走完,持有期不出帧、释放时才需一帧消费)与 `Pumped`(必须在渲染帧里推进,持有期必须持续出帧)。混用会让设计失去意义。
- `Ticket` 是 move-only RAII,析构即释放,`release()` 幂等;`consumeLanded` 是 exchange 语义、恰好消费一次——否则一次到货让循环永远跑下去(`Engine::requestRender` 踩过)。
- 接入是"对账式" `sync*WorkTicket` 而非配对 acquire/release(配对要求每条出错路径都记得放手,漏掉正是某次事故 `6028adcdf` 的形状)。
- **当前状态(2026-08-21 复核,Phase B 已收官)**:gating 已读账本 ——
  `Engine::needsFrame` 在 `kEnableWorkLedgerGating && frameRequestCallback_`
  时走 `ledgerGatingNeedsFrame`(Landing 落地消费一帧 / Pumped 持有期持续
  出帧 / 账本外相机自演进兜底),失败安全:未注入平台级唤醒钩子的平台
  (iOS/macOS 暂未接)回落旧 `hasConvergingWork`,零风险。`Scene::auditWorkLedger`
  保留为影子校验(每帧对拍令牌数 vs `Tileset::countTilesLoadingContent`,
  不等即 ERROR;账本 vs 旧判据分歧打 DIVERGE)。过渡态已结束,剩余是文档
  同步与更广真机 soak,见「还债路线」进度节。

### 3. 缓存分层:HttpCache(内存 LRU)+ PersistentCache(磁盘)
`HttpCache` 是线程安全内存 LRU(cesium `CachingAssetAccessor` 对应物,`maxEntries` 默认 2000),达上限先 `evictOne` 再在锁外 `persistAsync` 落盘。`PersistentCache` 是纯静态文件级落地,按 URL 哈希生成文件名。分层理由:内存层处理会话内高频命中,磁盘层处理跨会话持久化,写入丢到后台线程不阻塞主/网络线程。

### 4. Scene 用 Coordinator 族分解职责
`Scene` 自身只 own 5 个协调器(Layer/Tileset/Interaction/Environment/Telemetry)+ `SceneRenderPipeline` + `SceneFrameRuntime`。与 tiling "极端分解"哲学一致但目的不同:tiling 是为忠实移植 cesium 算法而拆,scene 是为让每个横切关注点(瓦片/图层/交互/环境/遥测)拥有独立可测边界、能各自 mock/替换。`Scene::update`/`render` 本身极薄,只做"构造 context struct → 转发给静态编排器"。`SceneFrameUpdateCoordinator` 是无状态静态编排器,按固定顺序:reset+framerate → camera update → build FrameState → tileset update。

### 5. SDK facade:一次性装配,不是持续管理
`installScene(EarthSceneConfig)` 按 `ImagerySourceKind`/`TerrainSourceKind` 分派构造 provider,按 `mvtSources` 创建 source-specific cache/pool/layer runtime,最终构造统一 `Tileset` 塞进 `engine_.setTileset`。facade 不做后续每帧管理——一旦返回,帧循环完全由 `Engine::render` 驱动：`Scene::update` 在本帧 camera/FrameState/tileset update 完成后自动更新所有 MVT source，`Scene::render` 再构建对应 FeatureRenderLayer 命令。**代价**:部分 provider 初始化路径是**阻塞式**的(TileMapService/WebMapService/BingMaps 的 GetCapabilities、GoogleMapTiles 建 session 用阻塞 POST,默认超时 20s)——是"一次性装配、调用方线程同步等待"设计选择的必然成本,不是 bug。

### MVT runtime 托管契约

`MvtSourceConfig` 是声明式配置入口，支持多个 source。默认每个 source 创建独立的解码/raw cache，因此不同 URL 使用相同 `z/x/y` 不会串源；需要与 MVT 面 drape 或兼容线场共享获取层时，调用方显式传入 `sharedCache`，且必须保证共享者属于同一 provider/request namespace。若未提供 fetcher，SDK 通过 `PlatformBridge::get` 执行 `{z}`/`{x}`/`{y}`/`{s}` 模板替换并持有取消句柄直到回调完成；注入 `fetchTile` 时，取消、超时与“最终一定回调”的契约由调用方承担。

Scene registry 的所有权单位不是裸 source，而是 source 与其 sink-bound `FeatureRenderLayer` 的 bundle。显式移除 source、重装 Scene 或销毁 Scene 时，完整退出顺序固定为：

```text
source.suspend()
→ 等待正在执行的 worker tessellation
→ drop GPU tile buckets
→ 移除 FeatureRenderLayer
→ 销毁 source/cache/pool
```

这保证 worker 不会在 layer 已销毁后继续解引用 sink。Surface teardown 走保留式协议：`source.suspend()` 等待 worker 并 drop GPU tile buckets，随后 layer 解绑旧 `RenderDevice`，但 source+layer bundle 仍保留在 Scene registry；Surface 重建时 layer 绑定新设备，source 由下一帧重新驱动。按需渲染的 `WorkLedger` 同时覆盖 MVT fetch/tessellation/commit/retry，避免异步结果到达后因宿主停帧而无人消费。

### 6. Engine 四阶段帧循环:beginFrame → update → render → endFrame
每阶段单独计时进 `Diagnostics`:
1. `device->beginFrame()` —— 渲染目标 + reverse-Z 深度清屏(near=150, far=1e12)。
2. `scene->update(dt)` —— 推进相机/控制器、更新环境(时间→太阳方向→天空渐变)、驱动 `Tileset::update()`。
3. `scene->render()` —— `SceneRenderPipeline` 按固定顺序建 RenderCommand 列表 → `renderer.submit` → `releaseRenderReferences()`(引用计数让瓦片存活到 GPU 消费完)。
4. `device->endFrame()` —— 呈现 drawable;`finishEngineFrame()` 记录总 CPU 帧耗时。
- **理由**:四阶段严格串行、职责单向(**update 不渲染、render 不做选择**),使每阶段可独立计时/测试,让"哪个阶段占用多少帧预算"有明确归属。

### 7. Scene 帧级统一资源仲裁

`SceneFrameResourceArbiter` 由每个 `SceneFrameRuntime` 独立持有，不是进程
singleton。它把 terrain、raster、MVT、`TerrainPageStore` 在同一帧对网络请求、
worker 派发、主线程 finalize、GPU 上传与 compose 派发的竞争收敛到同一张
producer×stage 配额表。各 stage 独立分配；`WorkerDispatch` 与 `ComposeDispatch`
都可能最终进入同一个 `AsyncSystem` 线程池，因此这里提供的是帧级准入与可观测性，
不是严格的共享线程池全局 QoS。
生命周期固定为：

```text
beginFrame
  → declareDemand(全部启用 producer)
  → sealAllocations(按 stage/priority/producer 分配)
  → tryAcquire(各执行点消费自己的 grant)
```

先收集、后分配的两阶段设计在 **producer 分配层** 避免 `primary → pending →
content → MVT → PageStore` 的调用先后决定谁吃满某个 stage 的预算。
Urgent/Normal/Preload 先按优先级分配；
terrain urgent 有显式保留额度，剩余额度按 producer 跨帧 round-robin，未使用
保留量会立即回流。`FrameResourceBudget` 仍是每个 Tileset 的局部保护与兼容计账
入口，但成功执行还必须消费 Scene grant，因此多个 Tileset 不再把帧总量按实例数
放大。

demand 不是按“系统已启用”固定声明整段满额：活跃 producer 每个相关 stage 只带
一个启动额度，上一帧实际消费量与 admission denial 会形成下一帧 hint，并被 stage
上限钳住。这样冷启动不会因零历史永久停住，持续积压会在一帧后扩容，而启用但
空闲的 MVT/PageStore/raster 不再长期占走大块不可借用 grant。snapshot 的
`deferred` 也会纳入真实执行点观察到的 admission denial，而不只看预声明与 grant
之差。

MVT producer 内另有 source 级公平：Scene 将 source 数量纳入首帧 demand，并以
source 为单位按剩余 grant 做 ceiling 分片，随后
每帧轮转起始 source。某个 source 未用完的份额会回流给后续 source；这层实例级
分片不改变 arbiter 的 producer×stage 总账，也不会把“本实例份额耗尽”误记成全局
admission denial。若实例份额耗尽但本地仍有候选工作，source 会记录独立的
instance-deferred 观察值；它进入 snapshot 与下一帧 hint，但不污染全局 denial 计数，
因此持续 backlog 仍会扩容而不会永久锁在 bootstrap 吞吐。Terrain producer 下的
多个 Tileset 目前仍按 `primary → pending → content` 固定顺序消费共享 grant，尚未
实现 Tileset 实例级轮转；Scene 总量不会按实例放大，但先后顺序仍可能影响实例间
延迟。

统一的是帧级 admission，不是生命周期或缓存实现：provider inflight、MVT cache、
PageStore 页池/compose 上限、GPU FIFO 等局部约束仍保留。被拒绝的工作必须留在
原队列下一帧重试；`GpuUpload` 的单位是逻辑事务，不是字节、耗时或 driver call：
一次 MVT 原子 LOD 置换可提交多个 child，PageStore 的 image/field indirection 事务
也可能包含多次 GPU 写入。各路径都在事务真正提交前扣款。PageStore 在没有 compose
worker 的测试/同步 fallback 中会就地执行，不消费 `ComposeDispatch`，生产异步路径
则受该 stage admission。`Scene`/`Engine` 暴露
`frameResourceArbiterSnapshot()`，可按 producer×stage×priority 观察 demand、
granted、used 与 deferred。

---

## 数据流(关键路径)

**Engine::render 四阶段**:`beginFrame`(device)→`update`(Scene→SceneFrameUpdateCoordinator→camera/environment/tileset)→`render`(Scene→SceneRenderPipeline→RenderCommand list→submit)→`endFrame`(device)。

**SDK 装配路径**:`installScene` → 按 config 各字段构造 provider/overlay → 构造统一 `Tileset` → `engine_.setTileset` → `Scene::setTileset` → `SceneTilesetCoordinator::setPrimary`。可选 glTF 内容走 `engine_.addTileset`。

**Scene::update 驱动各 coordinator**:`SceneFrameRuntime::makeFrameUpdateInput` 把 frameState/diagnostics/camera/controller/tilesets/timeController/skyGradient(按引用)打包成 `SceneFrameUpdateInput`,交给静态 `SceneFrameUpdateCoordinator::update`:①resetPerFrame+updateFrameRate ②cameraController.update ③build FrameState(含视锥构造、交互焦点 TTL、太阳方向/天空色) ④Scene resource begin/declare/seal ⑤tilesets.update(primary + content tilesets 共享同一 arbiter) ⑥`Scene::update` 继续驱动 MVT，`Engine::render` 随后驱动 PageStore，二者消费同一帧 grant。

---

## 关键契约与不变量

| 契约 | 说明 |
|---|---|
| WorkLedger `consumeLanded` 恰好消费一次 | exchange 语义;否则一次到货导致循环永认为"有活",出帧不停 |
| WorkLedger `Ticket` 双重释放让计数变负 | 判据从此恒说"空闲"=退回无 gating 冻屏——move-only + move 后源置空的设计原因 |
| Landing 与 Pumped 语义相反不可混用 | Landing 持有期不出帧、释放时才需一帧;Pumped 持有期必须持续出帧 |
| 对照测试即行为规格 | `tests/unit/geodesy/*` 中 `*MatchesCesiumNative` 用例定义的是"必须与 cesium 一致",改大地测量代码时是规格文档 |
| 异步回调只带按值/弱引用自持数据 | 全仓两起真实事故(裸引用悬空、裸指针)总结的通用法则;回调不能假设宿主还活着 |
| Scene 两阶段流分离 | `update(dt)` 只 mutate FrameState + 跑选择,`render()` 只读同一份 FrameState 建命令提交 |
| Scene resource allocation 必须先 seal 再执行 | 所有 producer 在执行前声明需求；seal 后不得追加 demand，也不得借用别的 producer 未消费 grant，避免调用顺序偏置 |
| admission denial 必须可跨帧续跑 | 请求、worker 任务、GPU 上传或原子换手被拒绝时留在所属队列/状态，不标失败、不拆事务、不丢占位内容 |
| 审计对拍 | `Scene::auditWorkLedger` 每帧比对令牌数与真值,不等即 ERROR——防漏接入新迁移点的静默失效 |

---

## 诚实得失

### ✅ 强项
- 大地测量/数学有系统性 cesium-native 对照测试守卫,独立审计结论"全部算法级对齐"。
- WorkLedger 把此前分散四处的在途判据收敛成单一账本 + 恰一次消费 + 每帧真值审计,解决了"画面冻住零报错"这个类别(已修过两次)。
- Scene 5-Coordinator 分解让横切关注点各自独立可测,`Scene::update`/`render` 足够薄。
- Engine 四阶段严格串行 + 逐阶段计时,诊断"帧预算花在哪"时归属明确。

### ⚠️ 短板 / 已知债
- **WorkLedger 已接管 gating(2026-08-21 复核)**:`Engine::needsFrame` 在
  `kEnableWorkLedgerGating && frameRequestCallback_` 时走 `ledgerGatingNeedsFrame`
  (见 §2 当前状态);`Scene::auditWorkLedger` 保留为影子校验,旧判据仅作
  未注入唤醒钩子平台(iOS/macOS)的失败安全回落。账本覆盖见「还债路线」
  进度节;残余风险 = 更广真机 soak 与 release 功耗量化,非代码缺口。
- **`PersistentCache::ensureDir` 是空函数体**,不真正创建目录——若调用方没预建缓存目录,磁盘落地会**静默全部失败**,无任何日志。
- **`PersistentCache::filePath` 用非加密、进程加盐哈希**做文件名,存在跨进程/跨平台不稳定与碰撞风险,不适合作长期稳定缓存键。
- **`PersistentCache::prewarm` 是纯占位符**(函数体空),只支持按需加载。
- **`AsyncSystem::Future::then` 每次起一个 detached 线程**(不经线程池),链式/扇出场景线程数不受控;审计记载全工程 0 调用(死代码,footgun 但无实际影响,建议删)。
- **`Uri::percentDecode` 末 2 字节 off-by-one**,`%XX` 落在字符串最后两位内不会被解码。
- **两段已确认死代码**:`SceneRenderCommandUniformUpdater` 里跳过 `owner=="globe"` 命令的分支与 `RenderCommandKind::SurfaceTile` 分支(globe fallback / SurfaceTileMesh 移除后残留,无生产者)。
- **`Diagnostics.h` 有一段"部分废弃"的 quadtree/surface 计数块**(旧 TileQuadTree/SurfaceTileMesh 遗留,仍声明但不再被填充)。
- **SDK facade 多处初始化阻塞调用方线程**(capabilities 抓取、GoogleMapTiles 建 session),默认超时 20s——设计选择的直接代价,未见异步版本入口。
- **缓存收敛策略与 cesium 存在结构性差距**(未验证是否已解决):缺请求侧护栏(cesium `RequestScheduler` 按屏幕优先级每帧重排 + 相机移走主动 cancel;我们 600 帧龄只是兜底),且我们是硬顶淘汰、cesium 是引用计数软预算——源头(HttpCache 淘汰策略)在 core/cache/。

---

## 还债路线:让 WorkLedger 接管 gating

> 背景见上节"⚠️ 短板"首条。这不是修 bug——是把原作者停在并行验证期的迁移走完。机制(Ticket/审计/`consumeLanded` 语义)已就位,工作量几乎全在 Phase A 的"枚举异步源"。

**进度(2026-08-17,Phase B 已收官)**:
- ✅ **Phase A**(`69b52412a`):`WorkTicketSlot` 幂等对账槽 + 矢量链两票(`mvtVectorLoad`/`mvtVectorCommit`)+ raster overlay 两票(`rasterOverlayLoad`/`rasterOverlayUpload`)+ `test_work_ledger` 3 条守卫。纯增量、零行为风险。
- ✅ **Phase B 翻转 + 平台级唤醒钩子**(`aa3715360`):`kEnableWorkLedgerGating=true`,失败安全(仅 host 注入唤醒钩子才走 ledger,否则回落旧判据)。`WorkLedger::setWakeCallback`(Landing 释放触发)+ `Engine::setFrameRequestCallback` + Android GLESView `wake()` 接线。
- ✅ **真机 soak 通过**(Android debug,2026-08-17):静置 6s 渲染 0 帧(真睡)/ 全程 WorkLedger DIVERGE=0 / `FrameGate: wake reason=workLanded` 端到端 / 停手截图内容自补齐(零冻屏)/ 收敛后再睡(无 ticket 泄漏)。
- ✅ **N 时钟/太阳**(`966bae234`):`advanceTime`/`setTime` 补 `requestRender("timeChanged")`(事件脏位,模式无关);固定钟 demo 零影响。
- ✅ **上传尾**(`b97fd851d`):`Tileset::update` 按 `gpuUploadQueue_.hasWork()` 对账 Pumped 令牌,替代 settle 依赖;demo 队列恒空零影响。
- ✅ **H 影像解码逸出**:复核 XYZ 系 `decodeTile` 内联于 callback 前、`inFlightRequests` 括住整段;Google/TMS 覆写但委托 XYZ 覆盖流程 → 无逸出。附:gap-audit 的 GoogleMapTiles availability UAF 已由 `availabilityMutex_` 修掉。
- ⏳ **余留(非阻塞)**:更广真机 soak(fly-to 飞行豁免路径 / 人为限速慢网 / 全球遍历)属持续观察;release 变体精确功耗数字待量(debug 已定性确认会睡)。

**账本现状(2026-08-21 复核,全仓六源)**:

| ledger label | Kind | 覆盖旧判据哪条 |
|---|---|---|
| `tileContentLoad` | Landing | ② 瓦片内容加载(`TilesetTile`,12 处状态迁移都接了) |
| `terrainPageUpload` | Pumped | ④ 页存储在途(`TerrainPageStore`) |
| `rasterOverlayLoad` | Landing | ③ raster overlay 网络在途(与 `rasterOverlayUpload` 配套) |
| `rasterOverlayUpload` | Pumped | ③ raster overlay 上传在途 |
| `mvtVectorLoad` | Landing | 矢量 fetch 在途(旧判据未列,Phase A 补上) |
| `mvtVectorCommit` | Pumped | 矢量 commit 在途(旧判据未列,Phase A 补上) |

对照 `hasConvergingWork` 的四判据:① 相机自演进**故意不进账本**(持续生产者
非"在途",由 `hasContinuousProducerWork` 单独兜底);②③④ 均有对应 ticket。
旧判据没列但在跑的异步(MVT 矢量 fetch/镶嵌、解码池)也已登记在册。

### Phase A —— 补齐账本覆盖,把审计打到静默(最花功夫)
`Scene::auditWorkLedger` 已是**找缺口的工具**:账本漏源时打 `verdict DIVERGE ledger=idle old=busy` + `audit MISMATCH`。
1. 开着审计跑齐场景:冷启动 / churn / 飞行 / pan 惯性 / 慢网挂下载。
2. 每条 DIVERGE 点名一个漏源。**已知至少要补**:raster overlay 异步(加 `Landing` ticket);逐一确认 MVT 矢量、worker 解码池。
3. 迭代到全场景审计静默——**这就是可验证的"达成信号"**(不是"感觉接全了",是"所有回放场景零 DIVERGE")。
   - ⚠️ 真风险:只在罕见场景出现的漏源(正是旧判据脆弱的同一原因)。审计只覆盖你真跑到的场景,场景清单要诚实列全。

### Phase B —— 把 gating 翻转成读账本
改 `Engine.cpp:279` 的判断为账本语义,注意**不是**纯 `anyOutstanding()`(相机自演进在账本外):
```
Pumped 有在途        → 必须继续出帧
consumeLanded 命中   → 出一帧消费落地产物
否则 camera 自演进   → 继续出帧(相机作为显式 gating 输入保留)
都不满足             → 休眠
```
⚠️ 翻转前必须钉死一个口径:旧判据 ② 用 `pendingRequests()`,审计对拍用 `countTilesLoadingContent()`/`tileContentLoad` ticket——**先确认三者数的是同一批**,否则会在略微不同的口径上翻转 gating,引入新的边缘漏判。

### Phase C —— 反转审计方向,再删旧判据
翻转后让 `hasConvergingWork` 反过来当影子校验(账本权威、旧判据检查),
soak 确认不再 DIVERGE,再删 ①②③④ 四段手写 if(相机那条抽成独立 gating
输入保留)。

**2026-08-21 状态**:Phase A/B 已收官(见上),`hasConvergingWork` 已降级为
影子校验 + 无唤醒钩子平台的回落路径(账本权威时 `auditWorkLedger` 打 DIVERGE
即为影子检查)。**暂不删旧判据**:iOS/macOS 未接 `setFrameRequestCallback`
唤醒钩子,回落路径仍需它;待两平台接钩子并 soak 通过后再删。Engine.cpp 的
"剩余 TODO(上传尾 settle 实测)"注释已过时 —— 上传尾已由 `b97fd851d` 收官,
注释随本次文档同步一并更新。

### 消除后的收益(按价值排序)
1. **省电(真正的 payoff)**:今天四判据把 Landing/Pumped 一视同仁当 Pumped,一个在飞网络请求就把满帧率按住其整个生命周期;翻转后 Landing 类挂着下载时可休眠、落地才醒一帧。慢网 + churn 的空转帧直接消掉。
2. **整类"静默冻屏"bug 被结构性关掉**(不只当前四源):今后新增任何异步生产者忘 gating,最坏与今天相同(不更差),而 RAII 让持有 ticket 成为默认、危险方向(永不入睡)是响的(awake reason 报 label)。失效方向从"默认坏"变成"要故意才坏"。
3. **单一事实源**:新异步工作 = 领一张 ticket,不再往手写四路判据加分支且指望每个 reviewer 记得更新。
4. **可诊断性(release 也能用)**:"引擎为什么醒着" 用 `outstandingByLabel()` 按 label 回答,而非一个粗糙 reason 字符串。

### 优先级判断
收益大头是省电 + 关掉一整类未来的坑,**不是修当前可见故障**(旧判据现在能工作)。优先级取决于:多在意移动端功耗,以及未来是否频繁新增异步源(新 provider/overlay/矢量源越多,收益 2 越大)。

---

## 扩展点
- **加新投影/CRS**:在 `Projection = std::variant<GeographicProjection, WebMercatorProjection>` 里新增成员类型,实现 `project`/`unproject`/`maximumGlobeRectangle`/`computeMaximumProjectedRectangle`,`Projection.cpp` 自由函数 visitor 会自动 `std::visit` 分派,不需改调用方。非投影类 CRS 变换(如国测局偏转)参照 `Gcj02CoordinateTransform` 的"独立显式转换点"路径。
- **加新缓存后端**(SQLite/LevelDB):改动点是 `PersistentCache` 的 `load`/`save`/`filePath`——但扩展前应先修 `ensureDir` 空实现与 `filePath` 哈希两个已知债,否则新后端继承同样的静默失败与碰撞风险。
- **加新 Coordinator**:定义 Coordinator 类 + 对应 Input/Context struct → 在 `Scene` own 实例 → 按需接入 `SceneFrameUpdateCoordinator`(update 阶段)或 `SceneRenderPipeline::Context`(render 阶段);`SceneFrameRuntime` 的 `makeFrameUpdateInput`/`makeInteractionContext` 是打包 context 的地方。
- **加新 SDK 配置项**:`EarthSceneConfig.h` 是纯声明式 POD;新增字段 → 在对应 struct 加字段 → 在 `installScene` 对应分派分支或辅助函数消费。新增一种 `ImagerySourceKind`/`TerrainSourceKind` 分支时留意:阻塞式初始化是现有约定,新分支引入网络抓取大概率沿用同一模式。

---

## 对照系
- **core 大地测量/数学/瓦片**:全面对齐 cesium-native——类型命名、算法步骤、容差常量、测试用例名(`*MatchesCesiumNative`)均系统性对照;真实差距在异步契约层不在算法层。
- **scene 装配**:自研,cesium-native 无直接对应物。Coordinator 分解模式、帧编排顺序都是本工程自己的设计。
- **相机/交互/环境**(附带提及):对齐 openglobus(见 `camera-interaction.md` / `environment.md`),与 core/scene 的 cesium 血统是"混血"关系。
