# cesium-native 平台无关层深度对齐审计（2026-07-04）

> 范围：`scaffold/src/earth_engine/` 除 `platform/` 外全部模块，对照 `/Users/ldy/Desktop/work/cesium-native`（@ bfc2c574c）。
> 方法：7 个并行深查（core 数学/大地测量、瓦片选择、raster overlay、terrain/content、scene/camera、async/utility、几何上采样）+ 1 个丢回调专项追查，全部高严重度结论经人工回源码二次核实。
> 结论先行：**算法层对齐度非常高**——SSE/雾化/kick/遍历、QM 解析/skirt、上采样裁剪/插值/UV、frustum/投影/相交测试全部确认一致。真实差距集中在**异步请求完成契约**这一个结构性维度上。

---

## 一、确认的真实差异（按严重度）

### 【高】请求完成契约：回调纪律 vs 保证性 continuation（结构性差异）

cesium-native 的在途计数递减挂在 Future 链的保证性 continuation 上（`TilesetContentManager::notifyTileDoneLoading`，异常/取消路径都不可能漏）。gis-md 的等价物是 `TilePendingRequestState::pendingRequests_` 集合（[TilePendingRequestState.cpp:20](../../scaffold/src/earth_engine/tiling/TilePendingRequestState.cpp)），释放**完全依赖 provider 在所有路径上恰好调用一次回调**，无任何兜底。任何一条丢回调路径 = cacheKey 永久滞留 → `hasNetworkInflightCapacity` 闸门（[TileLoadScheduler.h:141](../../scaffold/src/earth_engine/tiling/TileLoadScheduler.h)）永久 blocked + 该瓦片永不重试。

在这个契约下核实出三个具体薄弱点：

1. **QM metadata 聚合无兜底**（[QuantizedMeshTerrainProvider.cpp:1943-1978](../../scaffold/src/earth_engine/providers/QuantizedMeshTerrainProvider.cpp)）：瓦片内容 + N 个 metadata 子请求聚合，`remainingMetadata` 归零才触发主回调。任何一个子请求的 HTTP completion 被丢，主回调永不触发。
2. **共享 metadata 请求毒化窗口**（[QuantizedMeshTerrainProvider.cpp:1994-2002](../../scaffold/src/earth_engine/providers/QuantizedMeshTerrainProvider.cpp)）：同 URL 去重时后来者 `if (!inserted) return;` 直接挂到在途条目。若 HTTP 层对该 URL 丢过一次 completion，`inFlightMetadataRequests_` 条目永不清除，**之后所有需要该 URL 的瓦片静默挂死**——单次网络故障永久毒化一个 URL。
3. **ThreadPool 无异常防护**（[AsyncSystem.h:35](../../scaffold/src/earth_engine/core/async/AsyncSystem.h)）：worker 循环 `task()` 无 try/catch，任务抛异常直接 `std::terminate`。这是崩溃向量而非停滞向量，但同属"完成契约无兜底"family。

**与已知停滞 bug 的关系**（[[deep-zoom-load-stall-todo]]，2026-07-04 追查后修正）：机制 1/2 在**当前环境不可能是根因**——`collectUnderlyingLayerAvailabilityRequests` 只对声明了 `metadataAvailability` 的 layer 收集子请求，FABDEM 服务器的 layer.json（经典模式，`available` 一次给全 0-12 级）没有该字段且无多层链，`availabilityRequests` 恒为空，聚合计数器与共享 URL 去重两段代码在 demo 里休眠。这同时解释了实录 `cont 198/198 done` 的一致性。机制 1/2 降级为"接带 metadataAvailability 数据源（如 Cesium World Terrain）前必修"。停滞根因排查焦点回到内容请求主路径与 budget/派发条件。**建议下一步**：debug overlay 增加 `pendingRequests_.size()` 观测点（`inFlightMetadataRequests_.size()` 当前环境恒 0，可一并加上作 sanity check），停滞复现时定位是哪层卡住。

### 【中】

| # | 差异 | 位置 | 影响 |
|---|------|------|------|
| M1 | overlay 投影细节生成是"多帧动态优先级切换"（ModelBounds 优先、失败回退 Region），cesium 是单次原子映射 | [TileRasterOverlayDetailsGenerator.cpp:404-441](../../scaffold/src/earth_engine/tiling/TileRasterOverlayDetailsGenerator.cpp) | 回退发生时 overlay LOD 选择可能跨帧抖动；2026-07-03 的 ModelBounds 优先化已缓解主症状 |
| M2 | `RasterMappedToTilesetTile::loadThrottled` 只查 `_pLoadingTile==nullptr`，节流计数在 provider 侧异步恢复，晋升帧存在短暂超配窗口 | [RasterMappedToTilesetTile.cpp:530-542](../../scaffold/src/earth_engine/tiling/RasterMappedToTilesetTile.cpp) | 高覆盖场景瞬时超过 maximumSimultaneousTileLoads，内存/HTTP 峰值，不影响正确性 |
| M3 | ActivatedRasterOverlay 双 provider（headless placeholder + 正式）常驻，cesium 是 Promise 单 provider + 析构断言引用计数 | [ActivatedRasterOverlay.cpp:8-52](../../scaffold/src/earth_engine/layers/ActivatedRasterOverlay.cpp) | 反复启停 overlay 的长运行内存占用略高；功能正确 |
| M4 | B3DM `RTC_CENTER` → RTC extension 转换缺失（cesium B3dmToGltfConverter.cpp:168-183 有） | content/GltfContentProvider | 仅影响含 RTC 的第三方 3D Tiles；当前 QM 地形主线不受影响 |
| M5 | GeoJSON 解析无环闭合校验/洞最小点数校验/错误上报（cesium 校验+自动闭合+Result 携带 warning），GeometryCollection 无递归深度限制（cesium 限 8 层） | [GeoJsonParser.cpp:88-161](../../scaffold/src/earth_engine/data/GeoJsonParser.cpp) | 非法输入静默产出破碎几何；深嵌套可栈溢出（对抗性输入） |

已知技术债（本次确认现状未变，不重复展开）：FrameResourceBudget smoothing 公式未对齐；上采样 child bounds 粗一级 LOD（cesium 在 upsample 时同样不重算 elevation range，此点两边一致）。~~QM metadata 3 个边界测试失败~~——2026-07-04 复核证伪：QM/metadata 相关测试二进制全绿（terrain_provider 204 + metadata_availability 9 + parser_validation 8 + tileset_qm 28 + content_loader 15），系陈旧记载。

**2026-07-04 后续更新**：上文"高"级 1/2（metadata 聚合无兜底 + 共享 URL 毒丸）已根修（commit b198ce703，含析构后迟到 completion 的 UAF 修复），故障注入测试先复现后修复；3（ThreadPool try/catch）已修（bed25f510）。

### 【低】

- OBB 退化轴（degenerateAxes==2）用纯绝对 epsilon 逐分量比较，cesium 用 `equalsEpsilon`（相对+绝对）——对单位向量输入实际等价（[OrientedBoundingBox.h:119](../../scaffold/src/earth_engine/core/math/OrientedBoundingBox.h)）
- Ellipsoid 中心容差硬编码 `kEpsilon1` 而非成员变量——当前值相同，仅架构差异
- `Camera::getHeight()` 无地心奇异 optional 兜底（cesium ViewState 用 optional）——相机在地心才触发
- Frustum 用 6 平面（含 near/far），cesium culling volume 只用 4 平面——far 默认 1e12 m 等效无穷远，无实际过度剔除
- 帧编排：selection/load/unload 在一次 coordinator 调用内完成，cesium 分离 `updateViewGroup` 与 `loadTiles`——委托边界不同，未发现由此产生的行为分歧
- Frustum 仅 Gribb-Hartmann 单路径构造，cesium 另有几何构造法兜底退化矩阵

---

## 二、已证伪的发现（误报更正，防止未来重查）

| 代理原报 | 核实结论 |
|----------|----------|
| `computePixelRectangle` 缺显式 maxX/maxY，隐式宽高少 1-2px（报"高"） | **误报**。[RasterOverlayTileProvider.cpp:900-912](../../scaffold/src/earth_engine/providers/RasterOverlayTileProvider.cpp) 有与 cesium 完全一致的 roundUp(maxX/maxY) |
| `mapRasterTilesToGeometryTile` 缺 veryClose 边界容差 | **误报**。[RasterOverlayTileProvider.cpp:280-323](../../scaffold/src/earth_engine/providers/RasterOverlayTileProvider.cpp) 完整实现（含 /512 系数） |
| 点模式上采样严格不等式丢裁剪线上顶点（报"高"） | **误报**。cesium RasterOverlayUtilities.cpp:1071-1072 同样是严格 `> / <`，两边一致 |
| `Future::then()` detached 线程导致销毁竞态/主线程队列缺失（报"高"×3） | **大幅降级**。`Future::then` 全工程 **0 处调用**（死代码），引擎全部走 `pool().enqueue(callback)` 回调架构，不存在 cesium 式主线程 continuation 需求。`.then()` 建议删除或加警告注释防误用 |
| 加载派发缺 `numberOfTilesLoading` 闸门（报"高"） | **误报**。闸门存在于 [TileLoadScheduler.h:141](../../scaffold/src/earth_engine/tiling/TileLoadScheduler.h)；真实差异是闸门计数的**释放契约**（见高严重度节） |
| QM 加载缺 Y-up→Z-up 变换矩阵（报"中"） | **有意适配**。gis-md 顶点直接以 ECEF（Z-up）生成，根节点纯平移即可；cesium 嵌矩阵是因其按 glTF 规范走 Y-up 往返。真机已验证渲染正确 |
| viewport 高度无 max(1.0,·) 防护致 NaN | **降级为无实际影响**。SSE 公式无除以 viewport（乘法），高度 0 只得 SSE=0（退化帧本就不渲染） |
| ThreadPool `notify_one` 致线程饥饿 | **误报**。每 enqueue 一个任务唤醒一个 worker 是标准且正确的语义 |
| FOV 提取符号约定不一致 | **无实际影响**。`atan(±1/P[1][1])` 式提取全工程无使用 |

---

## 三、确认对齐清单（抽样验证通过）

- **core**：BoundingSphere/OBB（SAT、transform、contains、toSphere）、rayEllipsoid/rayTriangle（Möller-Trumbore 全分支）、rayPlane、AttributeCompression oct 编解码、ClipTriangleAtAxisAlignedThreshold、equalsEpsilon/negativePiToPi/zeroToTwoPi、Geographic/WebMercator project-unproject、cartesianToCartographic/scaleToGeodeticSurface
- **tiling 选择**：SSE 公式（abs vs 负号数学等价）、雾密度表插值与 exp 剔除、kick/additive/forbidHoles/loadingDescendantLimit、TileAvailability 位集、upsample 触发
- **terrain**：QM zigzag/反量化（÷32767 double 中间值）、highwatermark 索引、edge indices 不 zigzag、oct 法线、water mask、skirt 排序方向与 5.0 系数与 0.0001 偏移、layer.json available 解析
- **上采样**：先 U 后 V 裁剪序列、逐属性 lerp、skirt 高度减半规则、water mask 0.5 缩放+象限偏移、UV 重归一化（等价 cesium 删除重生成）、positionToTile 边界归属
- **scene**：Gribb-Hartmann 平面提取、view matrix 构造、reverse-Z 投影、ENU 三分支（地心/极点/常规）、多视锥架构、半空间符号约定

---

## 四、建议动作（按 ROI 排序）

1. **停滞 bug 诊断插桩**：overlay 加 `pendingRequests_.size()` + `inFlightMetadataRequests_.size()`，复现即定位（半小时级工作量，直接服务已知 P0）
2. **完成契约加兜底**：`completeSharedRequest` 用 RAII guard 保证 waiters 必被清算；metadata 聚合加超时/析构清算；ThreadPool `task()` 包 try/catch
3. **删除或封印 `Future::then()`**：死代码 + detached 线程 footgun
4. M2 节流窗口、M5 GeoJSON 校验、B3DM RTC_CENTER 按需排期
