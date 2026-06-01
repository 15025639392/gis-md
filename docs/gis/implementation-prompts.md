# 分阶段开发提示词

本文件提供可复制给 AI 的开发提示词。使用时应先让 AI 阅读 AGENTS.md 和本知识库，再执行具体阶段。

## 阶段 0：项目骨架

```text
请在当前仓库从 0 搭建 C++ 移动端地球引擎项目骨架。先阅读 AGENTS.md、docs/gis/earth-engine-roadmap.md、docs/gis/reference-architecture.md、docs/gis/core-api-contracts.md、docs/gis/mvp-acceptance.md、docs/gis/technology-decisions.md。

目标：
- 建立 src/core、src/renderer、src/scene、src/camera、src/tiling、src/providers、src/layers、src/interaction、src/debug、src/threading、src/platform/bridge。
- 配置 CMakeLists.txt（顶层 + src 静态库 + tests + 示例 app）。
- 配置 vcpkg.json（GLM、nlohmann/json、libcurl、GoogleTest、stb_image）。
- 实现 PlatformBridge 抽象接口。
- 实现 RenderDevice 抽象接口。
- iOS 示例：单屏 Metal view + CADisplayLink 渲染循环 + 基础 diagnostics overlay。
- Android 示例：单屏 GLSurfaceView + Choreographer 渲染循环 + 基础 diagnostics overlay。

不要实现复杂功能。完成后运行构建/测试，并说明目录职责。
```

## 阶段 1：核心数学

```text
请实现地球引擎核心数学与坐标模块。必须遵守 docs/gis/core-api-contracts.md、docs/gis/earth-coordinate-systems.md、docs/gis/engine-math-algorithms.md。

实现：
- Cartographic
- Vec3（封装 GLM::dvec3，内部 double precision）
- Mat4（封装 GLM::dmat4）
- Ray
- Rectangle
- Ellipsoid::WGS84
- cartographicToCartesian
- cartesianToCartographic
- screen/pick ray 所需基础接口

要求：
- 内部单位明确 degree/radian。
- 核心类型封装 GLM，但在头文件中不强制包含 GLM（通过 Pimpl 或 opaque pointer）。
- 提供 GoogleTest 单元测试，覆盖赤道、极区、高度非 0、degree/radian 误用。
```

## 阶段 2：可旋转地球

```text
请实现最小可渲染地球。遵守 docs/gis/rendering-engine.md、docs/gis/graphics-pipeline.md、docs/gis/interaction-system.md。

目标：
- 通过 RenderDevice 渲染 WGS84 椭球体或近似球体。
- 实现 Camera、Scene、Renderer、CameraController。
- iOS 通过 RenderDeviceMetal 实现。
- Android 通过 RenderDeviceGLES 实现。
- 支持触控拖动旋转和捏合缩放。
- 提供 diagnostics：FPS、draw calls。

验收：
- iOS 模拟器和 Android 模拟器首屏非空白。
- 相机不穿地。
- 近地和远地不明显抖动。
- 两个平台渲染结果视觉一致（截图对比）。
```

## 阶段 3：XYZ 底图

```text
请接入一个标准 XYZ Web Mercator 底图。必须阅读 docs/gis/tiles-terrain-lod.md、docs/gis/multi-tile-schemes.md、docs/gis/basemap-tile-rendering.md、docs/gis/data-provider-contracts.md。

实现：
- TileScheme
- TileKey
- ImageryProvider（通过 PlatformBridge HTTP 异步请求）
- BasemapLayer
- TilePlan / LayerTilePlan
- TileCache（raw data + decoded pixel buffer + GPU texture 三级缓存）
- parent fallback
- tile debug overlay

要求：
- 不要把 URL 模板当 TileScheme。
- 网络返回只更新 tile 状态，当前帧 render queue 决定画什么。
- 缓存 key 包含 provider/layer/style/version。
- 图片解码通过 PlatformBridge（iOS: CGImage、Android: BitmapFactory）。
- 纹理上传在主线程，通过 RenderDevice 创建。
```

## 阶段 4：Picking

```text
请实现基础 picking。必须遵守 docs/gis/interaction-system.md、docs/gis/engine-math-algorithms.md、docs/gis/core-api-contracts.md。

目标：
- screen -> pick ray
- ray -> ellipsoid intersection
- 返回 Cartographic 经纬高
- 在示例应用点击显示经纬度

要求：
- 正确处理 Retina/@2x/@3x 屏幕密度。
- 正确处理渲染 surface 尺寸和逻辑 viewport 尺寸的差异。
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

## 移动端验证要求

每个阶段完成后必须：

- 在 iOS 模拟器（iPhone 15 Pro 或等效）上运行并截图。
- 在 Android 模拟器（Pixel 7 或等效）上运行并截图。
- 两个平台截图对比，确认无系统性差异。
- 在至少一台真机（iOS 或 Android）上验证触控交互。
- GoogleTest 单元测试全绿。
