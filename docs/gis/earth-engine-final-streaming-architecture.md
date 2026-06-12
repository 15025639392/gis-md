# 地球引擎最终 Streaming 架构

本文定义 earth-md 地球引擎的长期目标形态，供 AI 和开发者在后续性能、瓦片、地形、渲染和线程改造中对齐边界。

核心结论：最终形态不是每帧全局追最新，而是维护一个稳定、渐进、可预算的地球 streaming pipeline。

## 目标体验

引擎优化必须优先满足用户感知，而不是追求所有屏幕区域同时达到最新 LOD：

- 拖动、缩放、旋转跟手。
- 视线中心和手势 anchor 附近先清楚。
- 近处先清楚。
- 边缘、地平线、离焦区域晚一点清楚。
- 不空白、不闪烁、不出现明显拼图感。
- 移动时稳定，停下后继续渐进变清楚。
- 请求、解码、tile plan、mesh、upload、eviction 都不能在单帧爆发。

不得把降低可见细节、缩短可见距离、默认放宽 SSE 当作性能优化主策略。优化应优先消除重复计算、无效请求、过度遍历、冗余上传、主线程阻塞和 GPU 提交开销。

## 最终流水线

```text
Input / Camera
  -> Frame Intent
  -> Tile Selection Snapshot      后台
  -> Layer Plan Snapshot          后台
  -> Request Scheduler            主线程决策 + 后台执行
  -> Decode / Prepare             后台
  -> Upload Budget Queue          主线程 / 渲染线程
  -> Renderable Tile Set          主线程采纳
  -> GPU Instance Buffers         渲染层
  -> Batched Surface Render       GPU
```

主线程不再每帧从零计算完整世界，而是维护一份当前可靠的可渲染状态，并在预算内小步采纳后台和 GPU 流水线产物。

## 三条优化线路

### 1. 主线程策略线

主线程负责当前帧体验、状态采纳、GPU 上下文和最终提交。

保留在主线程：

- 相机、手势、pick anchor、interaction focus 状态更新。
- 当前帧 `FrameIntent` 生成。
- 采纳或拒绝后台 `TilePlanSnapshot` / `LayerPlanSnapshot`。
- 中心、anchor、近处优先策略的最终仲裁。
- request token 发放和 inflight 限流。
- texture / buffer upload 预算执行。
- GPU resource 创建、销毁和 context/device lost 恢复。
- `RenderableTileSet` 切换和 fallback 决策。
- 父 tile、旧 tile、旧 mesh、旧 command 是否继续复用。
- instance buffer dirty range 更新。
- render submit。
- debug overlay 最终显示数据。

主线程不得做：

- 无预算全量 quadtree walk。
- 无预算全量 `LayerTilePlan` rebuild。
- 大量 request candidate 排序。
- CPU surface mesh 构建。
- unbounded texture / buffer upload。
- 等待后台结果。
- 因单个 tile 未完成而阻塞当前帧。

主线程每帧只回答一个问题：

```text
这一帧用哪些已经可靠的东西画出来，最多再接纳多少新东西？
```

主线程优化手段：

- 移动期 tilePlan 错峰和复用。
- request / upload / mesh / eviction 分帧预算。
- 中心、anchor、近处优先。
- 边缘、地平线、离焦区域延后。
- 上传和 GPU resource 创建节流。
- render command immutable 数据缓存。
- debug/diagnostics 降采样。

### 2. 后台异步线

后台负责 CPU 计算型、可基于快照、可丢弃的工作。

适合后台化：

- `TilePlan.compute`：quadtree walk、SSE、视锥、地平线判断。
- `LayerTilePlan.rebuild`：desired/render/request refs、fallback/kicking、readiness 统计。
- imagery request candidates 去重、过滤、排序。
- terrain request candidates 去重、过滤、排序。
- provider availability 预判。
- image decode、格式转换、mipmap/resize 预处理。
- quantized-mesh decode、height/normal/waterMask 预处理。
- surface mesh CPU build，如果仍保留 CPU mesh。
- cache eviction 候选收集和 LRU 排序。
- diagnostics 聚合统计。

后台输出必须是不可变 snapshot，不能直接改主线程状态：

```cpp
struct PlanSnapshot {
    uint64_t generation;
    CameraSnapshot camera;
    ViewportSnapshot viewport;
    TileSet visibleTiles;
    LayerPlan layerPlan;
    PriorityQueues requestQueues;
    double createdAtSeconds;
};
```

主线程采纳规则：

- generation 匹配。
- viewport 匹配。
- camera 没有过期太多。
- anchor/focus 仍相关。
- snapshot 比当前状态更有价值。
- 采纳成本在本帧预算内。

后台原则：

- 可以慢。
- 可以并发。
- 可以丢结果。
- 不能碰 GPU resource。
- 不能阻塞主线程。
- 不能直接写主线程 cache/map/vector。

### 3. GPU / 渲染层线

GPU 负责重复的 per-tile、per-vertex、批量数学和 draw/submit 成本下降。

最终 surface rendering 要从：

```text
一个 tile 一个 CPU mesh
一个 tile 一个 RenderCommand
一个 tile 一个 draw
```

演进到：

```text
shared grid mesh
per-tile instance data
少量 batch draw
shader 计算 ECEF / UV / normal / fog / light
```

推荐 instance 数据：

```cpp
struct SurfaceTileInstance {
    vec4 tileRect;       // west, south, east, north, radians
    vec4 textureRect;    // u0, v0, uScale, vScale
    vec3 localOrigin;    // ECEF origin for precision
    float opacity;
    uint textureIndex;
    uint flags;
};
```

GPU 侧承担：

- tile bounds -> 顶点经纬度。
- 经纬度 -> ECEF。
- texture UV 映射。
- geodetic normal。
- fog/light。
- transition opacity。
- water mask sampling。
- 后续 terrain height displacement。

GPU 化边界：

- CPU 仍负责高层 LOD、请求、fallback、资源生命周期。
- 第一阶段不要把 quadtree LOD 搬到 GPU，移动端兼容和调试成本太高。
- 先做 instanced surface rendering，降低 command 构建、submit 和 draw call。
- 纹理上传不能“移到 GPU”，只能减少上传次数、预算化、压缩格式或预处理。

## Renderable Tile Set

最终不应每帧从零构建完整 render list，而应维护稳定集合：

```cpp
struct RenderableTileSet {
    std::vector<RenderableTile> active;
    std::vector<RenderableTile> fadingIn;
    std::vector<RenderableTile> fadingOut;
    std::vector<RenderableTile> fallbackParents;
};
```

主线程每帧只做小步 mutation：

- add ready tile。
- replace parent with child。
- keep parent if child missing。
- fade transition。
- remove stale after TTL。
- update instance buffer dirty ranges。

这样用户看到的是连续状态，而不是每帧重拼一张地图。

## 统一 Streaming Queues

资源流入必须分层，并且每层都有预算、优先级和 stale discard。

```text
Request Queue
Decode Queue
Prepare Queue
Upload Queue
Activation Queue
Eviction Queue
```

每个任务必须携带：

- priority。
- generation。
- cancellation/stale token。
- owner layer/provider。
- estimated cost。
- created frame/time。

优先级围绕体验：

```text
priority =
  centerWeight
+ anchorWeight
+ nearWeight
+ screenSizeWeight
+ continuityWeight
- horizonPenalty
- edgePenalty
- stalePenalty
```

移动期策略：

- request 少发。
- upload 少传。
- mesh 少建。
- plan 错峰。
- 父/旧资源优先复用。
- 边缘和地平线延后。

静止期策略：

- request 增加。
- upload 增加。
- mesh / instance 补齐。
- 细节渐进完成。

## Cache / Eviction

Cache eviction 不应在交互热路径里全量扫描。

最终设计：

- 后台维护 eviction candidates。
- 主线程每帧释放少量 GPU resource。
- 移动期只做硬水位保护。
- 静止期做正常清理。
- GPU resource delete 分帧。
- tile 正在中心、anchor、可见、父 fallback 时禁止驱逐。

## Diagnostics

诊断不能成为热路径负担。

最终设计：

- 热路径只写 ring buffer counters。
- 后台聚合统计。
- overlay 低频刷新，例如 5Hz。
- 详细 trace 只在 profiling mode 开启。
- Android 真机日志采样输出，不每帧刷大量字符串。

## 阶段路线图

### Phase 1：主线程预算稳定

目标：任何阶段都不能单帧爆。

- tilePlan 错峰/复用。
- request/inflight cap。
- texture upload budget。
- surface mesh build budget。
- eviction 分帧。
- debug 降采样。

### Phase 2：后台 Snapshot

目标：拿掉 update 稳定成本。

- `TilePlan.compute` 后台 snapshot。
- `LayerTilePlan.rebuild` 后台 snapshot。
- request candidates 后台排序。
- 主线程只做 snapshot 采纳。

### Phase 3：RenderableTileSet 持久化

目标：不再每帧重建完整 render list。

- active/fading/fallback 集合。
- 小步 mutation。
- dirty range instance updates。
- 父子 handoff 状态机。

### Phase 4：Surface Instancing

目标：降低 command/draw/submit 成本。

- shared grid mesh。
- per-tile instance buffer。
- batch draw。
- texture array/atlas 或 texture bind 分桶。

### Phase 5：GPU Terrain Displacement

目标：减少 CPU surface mesh 构建。

- terrain height texture。
- vertex shader displacement。
- water mask / normal 数据纹理化。
- CPU 只准备高度数据，不按影像 tile 重建 mesh。

### Phase 6：统一 Streaming Scheduler

目标：request/decode/prepare/upload/activate/evict 全链路预算化。

- 多队列优先级。
- moving/idle 策略。
- generation/stale discard。
- 可观测指标和回归基准。

## AI 实施准则

AI 修改相关代码前必须先判断改动属于哪条线：

- 主线程策略线：预算、采纳、上传、提交、fallback。
- 后台异步线：计算、排序、解码、准备、候选生成。
- GPU/渲染层线：批量绘制、顶点数学、instance data、shader。

如果边界不清，先写诊断或 snapshot 接口，不要直接把复杂计算塞回主线程。

性能改动最终回复必须说明：

- 目标体验。
- 属于哪条优化线。
- 哪些工作仍留主线程。
- 哪些工作被后台化或 GPU 化。
- 帧预算和验证日志。
- 对中心/anchor/近处优先的影响。
- 对边缘/地平线延迟的取舍。

## 禁止项

- 用降低默认清晰度替代调度优化。
- 后台线程直接创建或销毁 GPU resource。
- 主线程等待后台 snapshot。
- 过期 snapshot 覆盖当前可渲染状态。
- 移动期全量扫描 cache eviction 候选。
- 每帧为每个 tile 从零创建完整 command 数据。
- 未预算地集中上传 texture/buffer。
- 用 debug overlay 遮盖 tile/LOD/terrain 问题。

## 相关文档

- `earth-engine-performance-foundation.md`
- `threading-architecture.md`
- `graphics-pipeline.md`
- `surface-tile-mainline.md`
- `basemap-tile-rendering.md`
- `tiles-terrain-lod.md`
- `debugging-observability.md`
