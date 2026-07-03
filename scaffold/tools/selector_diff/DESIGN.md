# Selector 对拍差分测试（gis-md ↔ cesium-native）

## 目标（可验证）

对同一合成瓦片树 + 同一相机帧序列，gis-md 的 `TileSelection*` 管线与
cesium-native（bfc2c574c）的 `Cesium3DTilesSelection::Tileset` 逐帧产出
**同构的选择轨迹文本**，并在 gis-md 单测中断言逐帧一致：

- 达成标准：最小场景（4 层四叉树 + 阶梯下降相机序列）全帧
  `render / counters / loads` 完全一致；任何不一致要么是 bug 修掉，
  要么进白名单并附 cesium 行号级论证。
- 回归价值：以后任何选择器改动跑该单测即可发现与 cesium 语义的漂移
  （2026-07-04 的 worldPositionForVertex 双重变换黑屏若有此测试当天即被抓住）。

## 架构

```
scaffold/tools/selector_diff/
  DESIGN.md                  ← 本文档
  scenarios.h                ← 共享场景参数（两侧 driver 各自 include/转写）
  golden/<scenario>.trace    ← cesium 侧生成的逐帧轨迹（提交入库）
  cesium_golden_gen/         ← golden 生成器（独立 CMake 工程，链接本地
                                cesium-native checkout 的 build-selectordiff）
scaffold/tests/unit/tiling/
  test_selector_cesium_golden_diff.cpp   ← gis-md 回放 + 逐帧断言
```

golden 由 cesium 工具离线生成后**提交入库**；gis-md 单测只读文件回放，
CI/日常跑测试不需要 cesium-native 构建。cesium-native 升级或场景变更时
重新生成 golden。

## 场景规格（两侧必须逐字段一致）

场景 = 纯参数（不逐瓦片枚举），两侧 driver 用相同规则展开：

- 瓦片树：以 region `[west,south,east,north]`（弧度）为根的满四叉树，
  深 `maxDepth`；子瓦片 = 等分四切（经纬各半）；`geometricError(z) =
  rootGeometricError / 2^z`；`refine = Replace`；包围体 = fromRegion
  （minH=0, maxH=0）。
- 加载模型：所有瓦片初始 Unloaded；帧 N 内被请求加载的瓦片在帧 N 结束时
  完成（cesium 侧 = 立即 resolve 的 mock loader + 帧末 `loadTiles()` +
  帧首 `dispatchMainThreadTasks()`；gis-md 侧 = 帧末把本帧 loadQueue 已
  发起的瓦片 `content.loadState = Done`）。即：帧 N 请求 → 帧 N+1 可渲染。
- 相机帧：每帧给 `(lon, lat, height)`（制图坐标），方向 = nadir
  （`-normalize(position)`），up = 本地北（退化时 Z），fovY、viewport 固定。
  两侧各自构 frustum/ViewState。
- 选项（显式设置，不依赖默认值）：`maximumScreenSpaceError=16`、
  frustum culling ON、fog culling ON（fogDensityTable 由场景显式给出，
  两侧同表）、occlusion OFF、LOD transition OFF、`forbidHoles=false`、
  `loadingDescendantLimit=20`、`maximumSimultaneousTileLoads=20`、
  `enforceCulledScreenSpaceError=true, culledScreenSpaceError=64`、
  preloadAncestors/Siblings ON。

## 轨迹格式（逐帧一行，纯文本，无 JSON 依赖）

```
frame=3 render=[2-0-1,2-1-1] loads=[3-0-2,3-1-2] visited=9 culled=4 culledVisited=0 kicked=0
```

- `render`：本帧选中渲染的瓦片 key（`z-x-y`），**排序后输出**（遍历序
  不作为对拍面）。
- `loads`：本帧实际发起加载的瓦片（cesium 侧 = mock loader 的
  `loadTileContent` 调用记录；gis-md 侧 = 本帧新发起的请求），
  **排序后输出**。加载优先级排序的对拍留待 phase 2（改为保序）。
- `visited/culled/culledVisited/kicked`：cesium `ViewUpdateResult` 的
  `tilesVisited/tilesCulled/culledTilesVisited/tilesKicked` ↔ gis-md
  `TileSelectionCounters` 的 `visited/culled/culledVisited/kicked`。

## 已知意图性差异白名单（对拍时须配置绕开或注释豁免）

1. gis-md 虚拟地形根（`TileSelectionRootPolicy::isVirtualTerrainRoot`）：
   场景根 key 避开虚拟根形态，或首帧豁免根瓦片。
2. gis-md `raw` selectionState 三处（TileSelection 对齐审计确认为忠实
   移植的 false positive）——不预期影响输出。
3. kick 两处语义 2026-07-02 已对齐 cesium，不豁免。
4. 遮挡剔除路径两侧关闭（gis-md 的 TileSoftwareOcclusionPolicy 是自研，
   非对拍面）。
5. gis-md 选择复用（TileSelectionReusePolicy）是 cesium 没有的优化：
   对拍时必须禁用（每帧全遍历），否则重复相机帧两侧 counters 必然分歧。
6. 视锥差异：cesium ViewState 剔除体无远平面；gis-md Frustum 取自
   viewProjection 含 near/far——gis-md driver 用极小 near/极大 far
   （0.1 / 1e8）使其在场景内永不参与剔除。

## P3（2026-07-04 启动）

- **loads 保序**：traceLine 的 loads 不再排序，两侧按发起顺序输出
  （cesium = mock loader 调用序；gis-md = TileLoadPriorityPolicy::
  sortByPriority 后的顺序）——优先级公式与排序语义入对拍面。
  s1/s2 golden 随格式变更重新生成。
- **S3**：S2 相机/选项 + kLoadDelayFrames=2（帧 N 请求 → 帧 N+2 选择
  前完成）——restore/kick 多帧渐进收敛面。
- **S4**：fog 边界。首版 nadir 方案证伪——cesium fog 剔除条件
  `exp(-(d·ρ)²)==0`（double 下溢）需 d·ρ≳27.3，nadir 低空视锥内无此
  距离；改为南缘低空 80° 俯仰朝北（800m→320km 穿越可达/临界/不可达
  三段）。cesium 把 fog 剔除计入 tilesCulled；gis-md trace 的 culled
  映射为 frustum+fog 之和。

白名单新增：

7. 并列加载优先级的顺序无语义（stdlib introsort 产物，跨实现可变）：
   场景层面消灭精确并列（相机经度偏离对称轴 +0.0037 rad），不做
   tie-breaker 对拍。设计新场景时避开对称轴。

## 首轮对拍战果（2026-07-04，P1/S1）

- render/loads/visited/culled/culledVisited 12 帧全部 byte 级一致，零豁免。
- 抓到并根修一处计数语义漂移：gis-md `counters.kicked` 原按 kick 事件
  +1，cesium `tilesKicked`（TilesetSelection.cpp:756）仅在
  notYetRenderableCount > loadingDescendantLimit 的 restore 分支按
  恢复移除的 load-queue 条数累加。已改
  TileSelectionPostTraversalCommitter 直接在 restore 分支累加并删除
  planPostTraversalCommit；对拍恢复全行严格比较。

## Phase 划分

- **P1（本次）**：S1 场景 = 4 层树、12 帧 nadir 阶梯下降相机（高→低），
  对拍 render+loads+4 计数器。
- P2：loads 保序对拍（优先级排序面）；加载延迟脚本（第 K 帧才完成）
  以覆盖 kick/ancestorMeetsSse 路径；fog 边界高度场景。
- P3：additive refine、forbidHoles=true、多 view、selection reuse 面。
