# 分阶段开发提示词

本文件提供可复制给 AI 的开发提示词。使用时应先让 AI 阅读 AGENTS.md 和本知识库，再执行具体阶段。

## 阶段 0：项目骨架

```text
请在当前仓库从 0 搭建地球引擎项目骨架。先阅读 AGENTS.md、docs/gis/earth-engine-roadmap.md、docs/gis/reference-architecture.md、docs/gis/core-api-contracts.md、docs/gis/mvp-acceptance.md。

目标：
- 建立 src/core、src/renderer、src/scene、src/camera、src/tiling、src/providers、src/layers、src/interaction、src/debug、examples/minimal-globe。
- 配置构建、测试和示例页面。
- 示例页面先显示空 canvas 和基础 diagnostics。

不要实现复杂功能。完成后运行测试/构建，并说明目录职责。
```

## 阶段 1：核心数学

```text
请实现地球引擎核心数学与坐标模块。必须遵守 docs/gis/core-api-contracts.md、docs/gis/earth-coordinate-systems.md、docs/gis/engine-math-algorithms.md。

实现：
- Cartographic
- Cartesian3
- Matrix4 或项目需要的矩阵类型
- Ray
- Rectangle
- Ellipsoid.WGS84
- cartographicToCartesian
- cartesianToCartographic
- screen/pick ray 所需基础接口

要求：
- 内部单位明确 degree/radian。
- 提供单元测试，覆盖赤道、极区、高度非 0、degree/radian 误用。
```

## 阶段 2：可旋转地球

```text
请实现最小可渲染地球。遵守 docs/gis/rendering-engine.md、docs/gis/graphics-pipeline.md、docs/gis/interaction-system.md。

目标：
- 渲染 WGS84 椭球体或近似球体。
- 实现 Camera、Scene、Renderer、CameraController。
- 支持拖动旋转和滚轮缩放。
- 提供 diagnostics：FPS、draw calls。

验收：
- 示例页面首屏非空白。
- 相机不穿地。
- 近地和远地不明显抖动。
```

## 阶段 3：XYZ 底图

```text
请接入一个标准 XYZ Web Mercator 底图。必须阅读 docs/gis/tiles-terrain-lod.md、docs/gis/multi-tile-schemes.md、docs/gis/basemap-tile-rendering.md、docs/gis/data-provider-contracts.md。

实现：
- TileScheme
- TileKey
- ImageryProvider
- BasemapLayer
- TilePlan / LayerTilePlan
- TileCache
- parent fallback
- tile debug overlay

要求：
- 不要把 URL 模板当 TileScheme。
- 网络返回只更新 tile 状态，当前帧 render queue 决定画什么。
- 缓存 key 包含 provider/layer/style/version。
```

## 阶段 4：Picking

```text
请实现基础 picking。必须遵守 docs/gis/interaction-system.md、docs/gis/engine-math-algorithms.md、docs/gis/core-api-contracts.md。

目标：
- screen -> pick ray
- ray -> ellipsoid intersection
- 返回 Cartographic 经纬高
- 在示例页面点击显示经纬度

要求：
- DPR 和 canvas 尺寸正确处理。
- 没有命中时返回结构化 none。
- 输出说明 CRS 和高度来源。
```

## 阶段 5：矢量叠加

```text
请实现 GeoJSON 点线面叠加层。必须遵守 docs/gis/overlay-styling.md、docs/gis/spatial-calculation.md、docs/gis/interaction-system.md。

目标：
- VectorProvider / VectorLayer
- Point/Line/Polygon 基础渲染
- OverlayStyle
- feature picking
- hover/selected 状态

要求：
- GeoJSON 坐标按 [lng, lat]。
- CRS 和高度模式明确。
- hover/selected 不导致整层重建。
```

## 阶段 6：地形

```text
请实现最小地形能力。阅读 docs/gis/tiles-terrain-lod.md、docs/gis/basemap-tile-rendering.md、docs/gis/graphics-pipeline.md。

目标：
- TerrainProvider
- TerrainTile
- TerrainMesh
- 影像贴地形
- terrain picking

要求：
- 高度基准和单位明确。
- 处理 tile skirt 或接缝策略。
- 地形缺失时有回退策略。
```

## 阶段 7：3D Tiles

```text
请实现 3D Tiles 最小 traversal。阅读 docs/gis/three-d-tiles.md、docs/gis/graphics-pipeline.md、docs/gis/engine-testing-acceptance.md。

目标：
- tileset.json 加载
- tile bounding volume
- geometricError / SSE
- content 生命周期
- 请求队列和卸载释放

要求：
- 不要把 traversal、网络请求、content 解析和 draw call 写在一个函数里。
- picking 至少能返回 tile/object 信息。
```

## 阶段 8：环境效果

```text
请实现最小环境系统。阅读 docs/gis/environment-atmosphere-weather.md、docs/gis/graphics-pipeline.md。

目标：
- Sky/background
- Sun direction
- 基础光照
- 可关闭的大气或雾效果
- TimeController

要求：
- 标明 visual-only / approximate / analytic。
- 环境效果不改变业务分析数据值。
```
