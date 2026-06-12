# 矢量瓦片地图展示与样式设计

矢量瓦片地图不是把图片瓦片贴到地球表面，而是把切片化的几何、属性和样式规则在客户端实时渲染成地图。它的核心价值是同一份数据可以按 zoom、属性、语言、主题和交互状态重新表达，并在高 DPR、旋转、倾斜和样式切换时保持清晰。

本文定义 earth-md 引入矢量瓦片底图时的产品目标、样式策略、渲染边界和性能契约。影像/栅格底图另见 `basemap-tile-rendering.md`，业务叠加样式另见 `overlay-styling.md`，通用性能契约另见 `earth-engine-performance-foundation.md`。

## 目标体验

矢量瓦片底图必须优先满足：

- 交互跟手：拖动、缩放、旋转和倾斜时相机反馈立即稳定。
- 渐进补齐：fill、line 和已有 symbol 可先显示，新瓦片和新标注可以异步补齐。
- 样式可控：地图主题、道路等级、文字、POI、边界和调试层可以通过样式系统配置。
- 可诊断：能看到瓦片状态、source-layer、feature count、style bucket、label collision 和性能队列。
- 不牺牲清晰度：优化优先消除无效工作，不默认降低可见细节、可见距离或必要反馈。

交互契约：

```text
用户输入必须优先于矢量瓦片后台工作。
PBF 解码、样式计算、文本布局、碰撞检测和 GPU 上传不能阻塞手势帧。
相机运动时允许延迟新增细节，但不得让已有可用地图大面积闪烁或清空。
```

## 推荐标准与生态

首版应采用 MapLibre / Mapbox Style v8 的受控子集，而不是自创完整样式体系。

推荐对齐对象：

- Mapbox Vector Tile specification：`.mvt` / `.pbf` 几何和属性编码。
- MapLibre Style Specification：`sources`、`layers`、`filter`、`layout`、`paint` 和 expression 结构。
- OpenMapTiles schema：道路、水系、建筑、POI、边界、土地覆盖等 source-layer 命名和属性分层。
- OGC API Tiles / Vector Tiles：服务能力、tile matrix、标准化接入方向。

不要求第一版完整兼容 MapLibre Style Spec。完整规范包含大量 symbol、文本 shaping、pattern、dash、表达式和平台细节，应该按本项目阶段逐步实现。

## 展示流水线

矢量瓦片在 globe 中的基本链路：

```text
FrameState
  -> VectorTilePlanBuilder
  -> VectorTileRequestScheduler
  -> MvtDecoder
  -> VectorTileStyler
  -> VectorTileBucketBuilder
  -> VectorTileUploadScheduler
  -> VectorTileRenderQueue
  -> Renderer
```

单个瓦片的数据流：

```text
MVT z/x/y
  -> PBF decode
  -> source-layer + feature properties
  -> tile-local coordinates by extent
  -> Web Mercator tile bounds
  -> lon/lat
  -> ellipsoid or terrain surface position
  -> style filter and paint/layout evaluation
  -> fill / line / symbol buckets
  -> GPU buffers and draw commands
```

`VectorTileRenderQueue` 只能绘制已经完成 GPU 准备的 bucket。网络回调、解码回调和样式回调只更新 tile 状态，不直接决定本帧最终画什么。

## 推荐模块边界

```text
VectorTileSource
  URL template, minzoom/maxzoom, attribution, tile scheme, cache key

VectorTileScheduler
  visible tile selection, priority, request limits, cancellation

MvtDecoder
  PBF decode, geometry command decode, extent/buffer handling

VectorTileStyle
  style JSON, layer order, filter, expression, paint/layout defaults

VectorTileBucketBuilder
  feature classification, triangulation, line mesh, label candidates

SymbolLayoutEngine
  glyph shaping, atlas use, screen-space placement, collision

VectorTileRenderer
  render buckets, state sorting, draw call batching, depth/blend policy

VectorTileDebugOverlay
  tile borders, z/x/y, feature count, queues, collision boxes, timings
```

不要把样式逻辑写进 Provider，也不要在 Renderer 中重新遍历 feature 计算样式。Provider 负责取数，Style 负责表达，BucketBuilder 负责把表达结果变成可绘制资源。

## 样式子集

首版建议支持：

```text
root:
version, name, sources, glyphs?, sprite?, layers

source:
type=vector, tiles, minzoom, maxzoom, bounds?, attribution?

layer:
id, type, source, source-layer, minzoom, maxzoom, filter, layout, paint

layer type:
background, fill, line, symbol-text
```

首版表达式建议支持：

```text
literal
get
zoom
==
!=
in
!
all
any
case
match
step
interpolate linear
```

暂缓支持：

```text
fill-pattern
line-pattern
line-gradient
复杂 line dash
沿线文字
复杂 text shaping
完整 icon sprite
fill-extrusion
heatmap
hillshade
跨 tile 全局 label placement
```

示例样式：

```json
{
  "version": 8,
  "sources": {
    "osm": {
      "type": "vector",
      "tiles": ["https://example.com/tiles/{z}/{x}/{y}.pbf"],
      "minzoom": 0,
      "maxzoom": 14
    }
  },
  "layers": [
    {
      "id": "water",
      "type": "fill",
      "source": "osm",
      "source-layer": "water",
      "paint": {
        "fill-color": "#78b7d8",
        "fill-opacity": 1.0
      }
    },
    {
      "id": "road-primary",
      "type": "line",
      "source": "osm",
      "source-layer": "transportation",
      "filter": ["==", ["get", "class"], "primary"],
      "paint": {
        "line-color": "#f2c36b",
        "line-width": ["interpolate", ["linear"], ["zoom"], 6, 0.6, 14, 5.0]
      }
    }
  ]
}
```

## Zoom 语义

样式必须明确不同 zoom 的信息密度：

| Zoom | 信息策略 |
| --- | --- |
| z0-z4 | 世界轮廓、海陆、水体、国家边界、大城市 |
| z5-z8 | 省/州边界、主要道路、主要城市、主要水系、土地覆盖 |
| z9-z12 | 城市结构、铁路、区县边界、主次道路、重要 POI |
| z13-z16 | 街道、路名、建筑轮廓、普通 POI |
| z17+ | 建筑细节、门牌、室内或非常精细 POI，按产品需要启用 |

低 zoom 不应把所有 feature 都交给 symbol 系统筛选。应在 style filter、source-layer、属性和优先级上提前减少候选量。

## 地球场景渲染策略

### Fill

面要支持 holes 和 multipolygon。首版可以贴椭球表面，地形 draping 后续独立实现。

必须处理：

- tile buffer 与边界接缝。
- ring orientation 和三角化失败 fallback。
- polygon offset 或深度偏移，避免与地表 z-fighting。
- 透明 fill 的 depth/blend 策略。

### Line

道路、边界和水系首版建议使用屏幕空间线宽。真实世界米制宽度在远处容易消失，屏幕像素宽度更符合地图底图体验。

必须明确：

- line width 单位为 px，按 zoom expression 变化。
- join/cap 的默认值。
- tile 边界处 line join 的接缝处理。
- 高倾斜视角下是否深度测试。

### Symbol

symbol 是独立子系统，不能阻塞 fill/line。

首版策略：

- 支持点标注和简单 POI label。
- 文本始终面向屏幕。
- 先做 tile 内碰撞，再做有限跨 tile 去重。
- 相机运动中保留已有 label，延后新增 label placement。
- 路名沿线排布延后。

label 必须有优先级，优先级至少来自：

```text
style layer order
feature rank / class
zoom
屏幕中心距离
是否已在上一帧稳定显示
```

## 性能边界

矢量瓦片性能边界必须从第一版开始内置，不能等出现卡顿后补救。

### 预算

初始 Android 真机预算：

| 指标 | 目标 |
| --- | --- |
| 交互 FPS | 优先 60 FPS，最低可接受 30 FPS |
| 主线程 update | <= 4-6 ms |
| render submit | <= 2-3 ms |
| PBF decode | 不在主线程 |
| style evaluation | 不在渲染帧全量执行 |
| GPU upload | 分帧限流，交互帧避免集中上传 |
| visible vector tiles | 有场景化上限 |
| resident vector tiles | 有内存预算和淘汰策略 |
| draw calls | 按 bucket 合批，可解释且稳定 |

### 队列上限

所有异步队列都必须有上限：

```text
maxNetworkRequests
maxDecodeQueueSize
maxStyleQueueSize
maxUploadBytesPerFrame
maxUploadBucketsPerFrame
maxTilesVisible
maxTilesResident
maxFeaturesPerTile
maxLabelsPerFrame
maxCollisionChecksPerFrame
```

超过上限时优先处理：

- 屏幕中心 tile。
- 当前 zoom tile。
- 面积大的 tile。
- 相机运动方向上的 tile。
- fill/line 高于 symbol。
- 已有 parent 或旧 zoom 可复用的 tile。

屏幕外、地平线外、背面、过时 generation 和 provider bounds 外的任务必须取消或丢弃。

### Tile 状态机

推荐状态：

```text
Idle
Loading
LoadedRaw
Decoded
Styled
GpuPending
GpuReady
Visible
Evicting
Failed
```

渲染层只消费 `GpuReady`。任何异步结果回到主状态表时都必须检查：

```text
tile key
source version
style version
camera/request generation
provider availability
```

过期结果不得上传 GPU，也不得替换当前可见结果。

### 样式缓存

禁止每帧对每个 feature 重新执行完整 filter 和 paint expression。

样式结果应按以下维度缓存或分桶：

```text
tile key
source-layer
style layer id
zoom bucket
feature class/rank/type
style version
```

style 切换时应区分：

- 只变颜色、透明度、线宽：尽量复用 geometry 和 buffer。
- 改变 filter、layout、text-field、symbol placement：需要重建对应 bucket。
- 改变 source-layer 或数据源：需要重新取数和解码。

### GPU 上传

GPU 上传必须由 `VectorTileUploadScheduler` 限流：

```text
每帧最多上传 N 个 bucket
每帧最多上传 M MB vertex/index/glyph 数据
交互帧使用较低预算
空闲帧可以补齐积压
```

禁止请求完成后在同一帧集中创建大量 buffer、texture 或 atlas 页面。

## Debug Overlay

首版必须提供基础观测项：

```text
FPS / frame time
visible vector tiles
resident vector tiles
network queue
decode queue
style queue
upload queue
draw calls
vertices / indices
feature count
label candidates
placed labels
collision checks
decode ms
style ms
layout ms
upload ms
render ms
memory estimate
```

首版必须提供诊断开关：

```text
show tile borders
show z/x/y
show source-layer
show feature count
show collision boxes
disable fill
disable line
disable symbol
freeze tile loading
freeze label placement
```

没有这些观测项时，任何“矢量瓦片性能瓶颈”的结论都只能标记为假设。

## 实施顺序

1. 定义 `VectorTileSource`、tile key、cache key、style version 和 tile 状态机。
2. 接入 MVT 请求、取消、raw cache 和 PBF decode。
3. 支持 `background`、`fill` 和 `line` 的最小样式子集。
4. 建立 fill/line bucket、GPU 上传限流和 render queue。
5. 增加 tile debug overlay、feature count、队列长度和阶段耗时。
6. 支持 zoom expression、filter 和 style bucket 缓存。
7. 增加基础 text symbol、glyph atlas、halo 和 tile 内碰撞。
8. 增加跨 tile label 去重、symbol 优先级和相机运动中的延迟布局。
9. 评估建筑拉伸、地形 draping、icon sprite、沿线文字和更多 MapLibre 子集。

## 验证方式

每次矢量瓦片能力改动至少覆盖：

- 固定城市中心俯视。
- 低空高倾斜道路密集区。
- 快速拖拽和缩放。
- 弱网或大量 tile 同时返回。
- style 热切换。
- symbol 开关 A/B。

记录：

- 截图或录屏。
- FPS 和 frame time。
- visible/resident tile 数。
- decode/style/layout/upload/render 耗时。
- draw call、vertex、label、collision 数。
- GPU 上传量和内存估算。
- Android 实机结果或说明未验证原因。

## 发布门禁

合入矢量瓦片相关默认能力前必须回答：

- 是否阻塞相机交互或主线程渲染帧？
- 是否有请求、解码、样式、上传和 symbol 队列上限？
- 是否支持过期任务取消和 generation 校验？
- 是否把 fill/line 与 symbol 分开调度？
- 是否避免每帧全量 feature 样式计算？
- 是否有 debug overlay 或日志能定位瓶颈阶段？
- 是否影响现有影像底图、SurfaceTile 主链路或地形渲染？
- 是否有固定场景 A/B 指标？

如果无法回答，改动只能作为实验能力，不能成为默认底图主路径。
