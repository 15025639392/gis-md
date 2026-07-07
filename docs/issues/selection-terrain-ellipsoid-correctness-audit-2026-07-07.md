# 瓦片选择 / 地形 / 高精度椭球 正确性审计（2026-07-07）

三路并行审计,逐项对照 `.ref/cesium-native/` 与 `.ref/cesium-js/` 源码(非凭记忆)。
范围:tile selection / LOD traversal、terrain(QM 解码/skirt/包围体/高度采样/upsample)、
高精度椭球与测地线。

## 总体结论

**三条路径的同步(默认)实现均为生产级忠实 cesium 移植。默认发布配置下无 P0/P1 正确性 bug。**
真正的正确性 delta 集中在:
1. 一个**可选异步路径**(`asyncSelectionNonBlocking`,默认关)——F1。
2. 一个椭球 memo 的**潜伏地雷**(F2,已修)。

本轮已落地两处修复(见下),其余为 P2 稳健性备注,记录备查。

---

## 已修复

### F1 — 异步 worker 静默关闭遮挡剔除(P1,仅 `asyncSelectionNonBlocking=true` 时激活)

- 真异步 worker 派发时 occlusion 回调传 `nullptr`
  ([TilesetSelectionFrameFacade.cpp:252](../../scaffold/src/earth_engine/tiling/TilesetSelectionFrameFacade.cpp)),
  `TileSelectionWorker::dispatch` 还 `assert(checkOcclusion == nullptr)`(worker 线程
  无法安全读取活体遮挡状态);shadow runner 回退到恒 `NotOccluded`。
- 但默认 `enableOcclusionCulling=true`、`delayRefinementForOcclusion=true`
  ([Tileset.h:65](../../scaffold/src/earth_engine/tiling/Tileset.h))。
- **后果**:cesium 对 occluded 瓦片强制 `action=Render/meetsSse=true`(渲染粗瓦片而非细化);
  异步 worker 会把这些瓦片继续细化成可见后代 → 异步渲染集与同步 cesium 集**发散**
  (正是 `occlusion-zero-horizon-point-patchwork` 记录的那类被遮挡区域过度细化)。
- **为何漏网**:golden 跑在 `enableOcclusionCulling=false`,且非阻塞异步路径本就无法逐帧
  字节对比 → 此回归无人看守。
- **根因合理性**:occlusion check 需访问 Tileset 活体状态,worker 线程不安全,故设计上
  故意丢弃——权衡非疏忽,但缺护栏。
- **修复**:在 `selectTilesAsyncShadow` 走 worker 分支前,若 `enableOcclusionCulling ||
  delayRefinementForOcclusion` 为真,打**每进程一次**的 Warning,显式暴露"两开关都开时
  遮挡被旁路"。不改变行为语义,只加可观测性护栏。

### F2 — `cameraCartographicMemoized` 的 key 不含 ellipsoid(P1 latent,已修)

- 未提交 diff 里新增的
  [TileBoundsMetrics.cpp](../../scaffold/src/earth_engine/tiling/TileBoundsMetrics.cpp)
  `cameraCartographicMemoized` 只用 `cameraPosition` 做 thread_local memo key,忽略
  `ellipsoid` 参数。
- 现两调用点都硬编码 `Ellipsoid::WGS84()`(单例),故当前无害。但任何未来非-WGS84 tileset
  (月球/火星/单位球测试)在相同相机位置命中缓存 → 返回上一个椭球的 cartographic,**数值错误**。
- **修复**:把 `ellipsoid.radii()`(`Vec3`,精确相等)并入 key。WGS84 路径 radii 恒等 →
  命中路径不变、golden 字节不变;消除潜伏地雷,代价仅一次 Vec3 比较。
- 其余对该 memo 的担忧均逐一验证为**不产生错误结果**:double 精确相等做 key(同 bit ⇒
  确定性同结果,worst case 只是 miss 重算)、跨帧陈旧(position 是 key,移动即 miss)、
  多视图交错(thread_local 无竞争,交错不同位置只降命中率)。

**验证**:`earth_engine_core` 编译通过;`test_selector_cesium_golden_diff`(10)、
`test_tile_selection_equivalence`(8)、`test_tile_selection_shadow_runner`(3)、
`test_bounding_region_builder`(11)全绿。

---

## 未修复 · P2 稳健性备注(正常使用不可见,记录备查)

### 异步路径陈旧滞后(F3/F4,P2,已有缓解)

异步 shadow 用**构建时快照**的内容 readiness,worker 晚 ≥1 帧落地 → refine/kick 拓扑反映
旧帧加载状态([TileSelectionShadowTree.cpp:44](../../scaffold/src/earth_engine/tiling/TileSelectionShadowTree.cpp))。
`reconcileShadowToLive` 用活体内容重新 gate 每个 render entry(防画黑瓦片),但**不修正**
refine/kick 拓扑 → 交互期瞬态 LOD 滞后 / 偶发洞。`previousSelectionState` 按 worker 完成
顺序而非帧顺序推进,快速交互下可能瞬时 kick 一个同步路径会保护的瓦片。异步设计可接受,但
确是与同步 cesium 的真实偏差,只在异步路径出现,golden 看不到。

### 地形

- **QM 顶点累加器截断成 uint16**
  ([QuantizedMeshParser.cpp:257](../../scaffold/src/earth_engine/terrain/QuantizedMeshParser.cpp)):
  cesium 保留 int32 running total。合规瓦片值域 `[0,32767]` 结果一致;**畸形瓦片**会
  mod-65536 回绕(单顶点飞到对边)。仅畸形数据可见。
- **`DecodedHeightmapSampler` 越界/no-data 返回 0.0**
  ([DecodedHeightmapSampler.cpp:29](../../scaffold/src/earth_engine/tiling/DecodedHeightmapSampler.cpp)):
  与真实海平面样本无法区分,cesium 返回 `undefined`。**⚠️ 更正(追调用链后)**:该 sampler
  是**死代码**——全仓库零源码消费方,未接相机。相机 clamp 实际走 `LoadedTerrainHeightSampler`
  (见下),故此条对相机无影响,仅是 sampler 自身 API 语义可议。
- **相机 clamp 无数据高地形瞬态钻地(P2/P3)——✅ 已修**:原相机下限 clamp 用
  `std::max(terrainHeight,0)+50m`,`terrainHeight` 来自 `LoadedTerrainHeightSampler`,后者无覆盖
  瓦片时返回 `0.0f`,与真海平面无法区分 → 低空快速进入**未加载/真空洞高地形**时相机被允许下到山体
  表面之下,瓦片加载后再顶回(dip→pop)。**海面本无问题**(海平面≈0,clamp 到椭球上方 50m 正确)。
  **修复**:让 sampler 区分无数据——
  - `LoadedTerrainHeightSampler::sampleHeightOptional`(返 `optional<float>`,无覆盖=nullopt)+
    保留 `sampleHeight` 便利包装(`.value_or(0)`,拾取等海平面兜底可接受的调用方续用);
  - `Tileset::sampleHeightOptional` / `SceneTerrainQuery::sampleHeight`(→`optional<double>`)/
    `CameraController::TerrainHeightFunc`(→`optional<double>`)整链传递;
  - `clampEyeToMinAltitude` 无数据(nullopt)时**保守回退到上一次有效样本**
    `lastKnownTerrainHeight_`(初值 0,有数据时更新)→ 下限稳定、消除 dip/pop。无地形 tileset 时
    缓存恒 0 → 行为等价旧的 bare-ellipsoid+50m(零回归)。
  - 新增测试:sampler nullopt 用例、相机"无数据保持上次高度不下沉"用例。全套 terrain/QM/selection/
    scene/camera 绿。
  cesium `sampleHeightMostDetailed` 是异步显式按瓦片查询、不这样 clamp 交互相机——本修复是自有 clamp
  设计下的最小正确化。
- **`sampleTriangleHeight` 用经纬度重心 + 跨瓦片取 max**
  ([LoadedTerrainHeightSampler.cpp:77](../../scaffold/src/earth_engine/tiling/LoadedTerrainHeightSampler.cpp)):
  高纬亚米级高度查询偏差,视觉无感。

### 椭球 / 测地线(约定/测度零差异,自洽)

- 测地线方位角归一化到 [0,2π);cesium 返回 [-π,π] 原始 atan2。物理方向相同,与 cesium
  期望值对比或 2π 环绕时会差整周。
- `sinSigma<1e-24` 早返回把重合点方位角塌成 0;cesium 继续循环算出定义良好的 heading。
- `cartesianToCartographic` 高度符号在 `dot==0` 边界:我们保 `+length`(此时约 0),cesium
  `sign(0)*mag=0`。仅测度零边界。

---

## 已逐项验证为忠实(勿重查)

**椭球**:Newton `tryScaleToGeodeticSurface`(cesium 同样无迭代上限,NaN 时 `NaN>eps` 为假
即退出,无死循环)、`tryCartesianToCartographic` 高度符号逻辑、`cartographicToCartesian` /
`geodeticSurfaceNormal`、ray-ellipsoid 数值稳定技巧(`root0=temp/w2; root1=diff/temp`)、
WGS84 半短轴 `6356752.3142451793`、Vincenty inverse/direct 级数(1000 迭代上限是 cesium 无
上限的安全超集)。

**选择**:SSE 投影公式无 off-by-one(距离 `max(d,1e-7)` guard 一致)、region 距离平面投影
(west/east/south/north 平面点积 + 高度 clamp + OBB max)、kick 逻辑(含两处已注释的刻意对齐:
非-additive kick 恒 `addReplacementToPlan`、无条件 `state=Rendered`、`notYetRenderableCount`
在 restore 分支重置为 `renderable?0:1`)、frustum→fog 剔除顺序 + `cullWithChildrenBounds`、
additive/forbidHoles/ancestorMeetsSse、`mustContinueRefiningToDeeperTiles`→urgent load。
同步-shadow 路径有 §8 等价 oracle(`TileSelectionEquivalence.h`)+ golden 逐帧字节守护。

**地形**:zig-zag/index(high-water-mark)解码、u/v/h 反量化 + ECEF 构造、边索引读序、
metadata(extId 4)/water-mask(extId 2)扩展解析、octDecode、skirt 高度公式
(`5.0*(maxRadius*0.25/65)*width`)与四边生成(partial_sort + strip)、BoundingRegion OBB
(窄 `width≤π` 切平面 / 宽 `width>π` 赤道对齐 / 极点特例)、BoundingRegion 距离、upsampler
(Sutherland-Hodgman 双轴裁剪 + SE/SW/NE/NW 象限窗口 + 边收集 + skirt 重降 + water-mask 0.5
缩放 + 顶点属性 lerp)、EllipsoidTerrain 栅格。

---

## 建议后续(未做)

- F3/F4 若交互期 LOD 滞后碍眼,考虑给异步路径加增量 readiness 快照(大工程,当前可接受)。
- `DecodedHeightmapSampler` no-data 语义:审计相机 clamp 调用方是否把 0.0 当忽略,否则改为
  返回 `optional<float>` 显式区分。
