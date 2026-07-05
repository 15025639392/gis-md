# Selector 增量切面(Incremental Frontier)设计

**日期**:2026-07-06
**状态**:设计完成,feasibility ✅ 已判定(§6b closure 定稿 + additive 确认不触发);未动代码。下一步 = Phase 0 flag + §8 等价性基座。
**目标**:把 `selectTiles` 每帧成本从 `O(可见瓦片)` 降到 `O(边界瓦片 + 事件驱动 dirty 瓦片)`,把真机拖动 selector 从 24–37ms 压到 ~3–7ms 量级。
**前提**:本文档只负责**把成败难点在纸上锁死**。写不清楚 §6 的两个难点,就不该动代码。

---

## 1. 为什么走到这一步(已穷尽的路)

拖动时 `selectTiles` 是真机头号开销(24–37ms,`raster-mapping-three-gates-2026-07-06`)。此前所有「轴 A:降低每瓦片常数成本」的手段都已到底:

| 手段 | 状态 |
|---|---|
| region planes / OBB / 相机 cartographic memoize | 已提交(`b1e745594`/`e6b6330f2`),trig 已全部提取 |
| per-tile distances scratch 复用 | 已提交(`4b8224e5d`),消除每瓦片堆分配 |
| SoA 布局(策略3-①) | **证伪**:贵活已被上面的 memoize 覆盖,纯 DFS 下 SoA 仅剩测不出的 cache 局部性 + 逐位一致风险 |
| async worker 移线程 | 已实现但**体感变差还回**(结果延迟 ≥1 帧,`raster-mapping-three-gates-2026-07-06`) |

per-tile 分项实测:vis 1.4ms(已查证不可改) + **遍历机器 1.8ms(refine/kick/后遍历)** + distance/SSE/priority 0.8ms(trig 已省)。**遍历机器那 1.8ms 是控制流,任何 SoA/SIMD 都够不着。**

唯一剩下的数量级杠杆 = **轴 B:减少每帧评估的瓦片数量**。这也是唯一能吃掉遍历机器 1.8ms 的路(跳过稳定内部 = 连 refine/kick 都不跑)。

---

## 2. 核心原理

### 2.1 严格上界(非近似)

对任何固定瓦片,三角不等式给出:

```
| d_new(tile) − d_old(tile) |  ≤  | camera_new − camera_old |  =: δ
```

### 2.2 SSE 只依赖 distance(交互期)

`InputMetrics.cpp:51` 的 SSE 展开为闭式:

```
SSE(tile) = geometricError · k / d ,   k = proj[1][1] · viewportHeight / 2
```

`k` 只含 **FOV** 和**视口高**。已核实(`Scene.cpp:31` 一次性设 FOV,`CameraController.cpp` 缩放纯 dolly 移位置):**交互期 FOV 与视口恒定** → `k` 是常数 → SSE 唯一变量是 `d`。near/far 不进 SSE 式。

### 2.3 翻转带(可跳过判据)

设决策阈值 `T = maximumScreenSpaceError`。恰好满足 SSE 的距离 `d* = gE·k/T`(逐瓦片不同,gE 每层减半 → d* 每层减半 → 切面天然是同心壳)。

下一帧 `d_new ∈ [d₀−δ, d₀+δ]`,故 `SSE_new ∈ [gE·k/(d₀+δ), gE·k/(d₀−δ)]`。**决策翻转 ⟺ 该区间跨过 T**:

- `gE·k/(d₀+δ) > T`(最远也仍需细分)→ **稳定,跳过**
- `gE·k/(d₀−δ) < T`(最近也仍满足)→ **稳定,跳过**
- 否则 → **进重评估集**

几何直观:**只有 `d₀` 落在以 `d*` 为中心、半径 ~δ 的距离带里的瓦片需要重算**。δ 决定带宽,拖动越快带越宽。

> 注:双阈值。剔除瓦片走 `culledScreenSpaceError`(更松),同一判据对两个阈值各算一次带。

---

## 3. 与已失败的 stale-reuse 的本质区别(必读)

`drag-jank-selector-and-async-design` 记录的 stale-reuse 被否决,原因是它**整体跳过 selection**:新区域瓦片永不进 `loadQueue` → 永不加载 → **黑屏**。

增量切面**不是跳过**:它**每帧都运行**,只是用 §2 的不等式证明大部分瓦片不必重算。边界一直推进,新区域一进带就被评估、下钻、入 `loadQueue`。**它从不停止推导 loadQueue** → 那个黑屏根因不复现。这是它区别于 stale-reuse 的关键。

---

## 4. 数据结构

- **持久 frontier**:上帧的选中瓦片集(TileKey → 状态),跨帧存活于 `Tileset`。
- **margin 索引**:按「到阈值的 SSE margin」组织的 bucket/heap,使每帧能 `O(边界数)` 取出带内瓦片,而非扫全体。margin 符号区分内侧(有余量可变粗)/外侧(需细分)。
- **frustum 边界环**:frontier 中位于视锥边缘的瓦片子集(§6a 用)。
- **dirty 集**:相机无关的异步事件(加载完成、遮挡返回)标记的重评估瓦片(§6b 用)。

registry tile 对象**从不删除**(只卸载 content),故持久 frontier 持 `TileKey`(或稳定 tile*)安全——同 async 影子树的降险事实。

---

## 5. 每帧算法(骨架)

```
onFrame(camera):
  if 投影矩阵变了(resize / 未来的光学变焦):   # 极少
      full_traverse(); rebuild_frontier(); return   # 退化,成本可忽略
  δ = |camera_new − camera_old|
  candidates = margin_index.take_within_band(δ)      # 相机驱动
             ∪ dirty_set.drain()                     # 事件驱动(§6b)
             ∪ frustum_boundary_ring                 # 视锥平移(§6a)
  for tile in candidates:                            # O(边界), 不是 O(可见)
      重算 distance/SSE(复用现有 prepare 路径)
      if 翻转到「需细分」: 下钻一层, 孩子入 candidates 下一轮, 入 loadQueue
      if 翻转到「可变粗」: 回收孩子, 父入 frontier
      更新该 tile 的 margin, re-insert
  incrementally_update_counters()                    # §6b: kick/notYetRenderable 等
  # 稳定内部瓦片: 一个都不碰
```

---

## 6. 两个决定成败的难点(feasibility 就卡在这)

### 6a. 视锥平移的新增/移除瓦片

相机平移使一侧新瓦片进锥、另一侧出锥。增量集不能只看已有 frontier。
- **移除**:每帧对 frustum 边界环做 frustum 测试,出锥的从 frontier 移除。
- **新增**:出锥/进锥发生在 frontier 的**空间邻居**。用 scheme 的相邻关系(同级 x±1/y±1)把边界环瓦片的未选中邻居拉入 candidates。
- **风险**:边界环的定义与维护若不严谨,会漏掉快速平移带来的整片新区域。**Phase 3 专门验证**(快拖到新区域截图必须满屏无洞)。

### 6b. 有状态内部逻辑的增量维护(**最硬,feasibility 在此判定**)—— 已定稿(2026-07-06 读码枚举)

**决策输入三分类**(读 `TileSelectionTraversalExecutor::visitTile` + `RefineFlowPolicy` + `PostTraversalPolicy` + `KickPolicy` 得):

| 决策输入 | 类别 | 变化来源 | 处理 |
|---|---|---|---|
| frustum / fog / SSE / viewerRequestVolume | **G 相机几何** | 相机移动 | §2 margin 带 |
| ancestorMeetsSse | 传播 | 父→子沿途 | 重评估子树时随传 |
| previousSelectionState / wasRenderedLastFrame / childWasRefinedLastFrame | **S 自身跨帧史** | 该瓦片自己 | 见下「S 自洽」 |
| renderable(内容+raster 就绪) | **A 异步** | 内容加载完成 | dirty 触发 |
| occlusion result | **A 异步** | 遮挡回调返回 | dirty 触发 |
| canRefine / ensureTileChildren.retryLater | **A 异步** | 子可用性加载 | dirty 触发 |
| traversalDetails.{allAreRenderable, notYetRenderableCount, anyWereRenderedLastFrame} | **A 聚合上卷** | 后代加载/渲染态 | 沿祖先链增量聚合 |
| lodTransitionFadePercentage | 每帧动画 | fade(**默认关**) | fade 开时恒重评估 |

**S 自洽**:previous/wasRendered/childWasRefined 都是瓦片**自己的**跨帧历史。维护 frontier 时,每帧对 touched 瓦片推进 `previous←current`;**skipped 瓦片状态不变 = 自洽**(它确实没被重选,历史本就不该变)。需要一个 O(frontier) 的轻量 previous←current 推进(纯 enum 拷贝)或惰性版。

**A 的 dirty 传播规则(closure,已由 `KickPolicy.cpp:5-29` 证明有界)**:
- `shouldKickDescendants` 读的是 `traversalDetails` 的**后代聚合量**(allAreRenderable / notYetRenderableCount / anyWereRenderedLastFrame)。
- 故:**瓦片 T 的 renderable / occlusion / canRefine 异步变化 → dirty(T) ∪ 沿 T 的祖先链上卷**(每个祖先增量更新聚合量 + 重跑 kick 判定),`O(depth)`/事件。
- **关键正确性论证**:父的 kick / 「等兄弟组(complete-renderable)」判定依赖全部子状态;子 A 变时,重评估父 = 用「更新的 A + **缓存的兄弟状态**」重新聚合 → 结果正确。**async 事件影响半径 = 自身 + 祖先链,不外溢到兄弟子树**(兄弟状态未变,缓存即真值)。
- 故 **无需 dirty 兄弟,只 dirty 祖先链** → closure 有界、可枚举。

**feasibility 判定:✅ 对 replace-refine + fade-off(即设备默认配置)可行。** closure = {相机→margin 带} ∪ {async(load/occlusion/avail)→自身+祖先链} ∪ {fade 开→fading 瓦片恒重评估}。三源均有界且已逐条对应到代码。§8 等价性测试仍是最终防线(暴露枚举漏项),但纸面枚举已闭合,不是开放风险。

### 6c. additive refine —— 已确认线上不触发

已核实:默认 `refine=Replace`(TilesetTile.h:35);`TileRefine::Add` **仅** `GltfContentProvider.cpp:4074`(3D-Tiles glTF 数据集)设置,terrain/QM provider 从不产生。**当前 globe 用例(QM 地形 + raster 影像)纯 replace-refine,additive 零触发。** 设计上:遍历中遇任一 additive 瓦片,该子树退化回全量(守卫已足够,当前数据走不到)。Phase 4 再补 additive 增量。

---

## 7. 分阶段落地(每阶段都有二元 gate)

### ⚠️ 关键修正(2026-07-06 实现期):dirty 集不是可延后的 Phase 2

原计划「Phase 1 纯相机驱动、dirty 集延后」**在拖动场景下不成立**。真机拖动 `notReady≈200`——内容每帧异步加载,`renderable` 在全树翻动。纯相机剪枝(不管 dirty)会拼接 stale 子树 → **§8 oracle 必然拒绝**。纯相机剪枝只在**收敛场景**(无加载)可行,而收敛时现有 reuse gate 已经复用了。**故 async dirty 失效是「第一个有用增量」的必需项,不能延后。** 拖动增量的真实结构 = margin 带(相机)∪ dirty 集(async),二者缺一则 oracle 红。

### 实现按「oracle 验证的层」推进(每层默认关、golden 不变)

所有增量机器 gated 于 `incrementalSelection`(默认关),独立于全量路径;golden 测全量、增量测靠 §8 oracle(归一化比较,不与 golden trace 的 load issue-order 耦合)。

| Layer | 内容 | 收益 | oracle gate |
|---|---|---|---|
| **L0** | `incrementalSelection` 路由到 `selectTilesIncremental`,当前恒 delegate 全量(identity)| 无(建入口)| 增量==全量 平凡通过 |
| **L1** | 子树贡献缓存 + 遍历时捕获(visibleTiles/loadQueue 区间 + counter delta + traversalDetails + 子树 min-margin);**每帧全量重算不剪枝** | 无(建数据结构)| 增量==全量(缓存不影响输出)|
| **L2** | dirty 集接线:async(renderable/occlusion/canRefine)完成 → 沿祖先链标 dirty;margin 带 → 相机驱动 dirty | 无(建失效)| 增量==全量(dirty 全标=退化全量)|
| **L3** | 开启剪枝:clean(非 dirty + margin 稳)子树跳过重算、拼接缓存贡献 | **✅ 首个提速**:跳过稳定子树的遍历机器 + 几何 | 增量==全量(漏 dirty→oracle 红)|
| **L4** | 视锥平移新增/移除(§6a)+ additive 守卫 + 真机 A/B | 覆盖快拖 | 真机 selector A/B + 截图无洞 |

**L0-L2 是正确但不提速的脚手架(输出恒等于全量);L3 才出速度**。每层 oracle 绿才进下一层。收益预期:收敛/慢拖(已载区)大;快拖到新区(全churn)小——诚实边界见 §9。

**贯穿全程的验证基座(§8)**:每层 gate 都靠它。

---

## 8. 等价性测试基座(唯一防线)

字节级 cesium golden 对拍**不再适用**(cesium 每帧全量,我们增量 → 架构偏离)。替换为**「增量 == 自身全量」对拍**:

- debug/test-only:每帧在增量选择后,**再跑一遍全量选择**,断言 `visibleTiles / loadQueue / counters` 逐位相同。
- 用**多帧扰动序列**(平移 + 缩放 + 加载完成事件 + 遮挡事件的组合)驱动,覆盖 §6b 闭包。
- 复用已有 async 影子树基建(`TileSelectionShadowRunner` 能在影子上跑全量,现成)。
- release 关闭(零开销)。

这个基座是 ③ 能否安全 ship 的前提:**它红,就是 §6b 闭包漏了项**。

---

## 9. 预期收益 / 风险 / 回退

- **收益**:边界带占 10–20% → selector 24–37ms → 3–7ms 量级,且省遍历机器 1.8ms(SoA 够不到的部分)。数量级级别,远超轴 A 的常数因子。
- **风险**:§6b 有状态内部维护是 bug 温床;§8 等价性测试是唯一防线。
- **回退**:kill-switch flag,任何阶段可退回全量,零风险并存。
- **为什么 cesium 不做**:cesium 赌 per-tile 便宜、每帧全量重算(简单、无状态维护风险)。我们 per-tile 已到底但仍不够(移动端 + 深瓦片),所以必须走 cesium 没走的路——这是**有意识的架构偏离**,不是偏差。

---

## 10. 开工前的先决条件

1. 确认线上数据集无 additive refine(否则 §6c 提前)。
2. Phase 0 flag + §8 等价性基座**先落地**(在写任何增量逻辑之前),否则无法验证正确性。
3. §6b 的「受影响集闭包」先在纸上逐条列全(kick/occlusion/complete-renderable/notYetRenderable 各自的触发源 → dirty 传播规则),再动代码。
