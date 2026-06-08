# 从 0 开发任务级拆解

本文件把路线图拆成可交给 AI 执行的任务粒度。每个任务都应该能独立开发、测试和验收。

## 任务格式

每个任务必须包含：

- 目标。
- 前置条件。
- 修改模块。
- 核心接口。
- 测试。
- 验收。
- 禁止事项。

## T00 工程脚手架

目标：

- 建立 C++17 / CMake 项目骨架。
- 配置 vcpkg 依赖（GLM、nlohmann/json、libcurl、GoogleTest、stb_image）。
- 提供 iOS 和 Android 最小示例应用（空渲染 surface + 基础 diagnostics）。
- 配置 CMake toolchain（iOS cross-compile、Android NDK cross-compile）。

验收：

- `cmake --build .` 成功输出 `libearth_engine_core.a`。
- iOS 模拟器可启动并显示空白 Metal view。
- Android 模拟器或设备可启动并显示空白 GL surface。
- GoogleTest 单元测试可执行。

## T01 基础数学类型

目标：

- 实现 `Vec3`、`Cartographic`、`Mat4`、`Ray`、`Rectangle`。
- 配合 GLM 使用（核心类型封装 GLM，不直接暴露 GLM 头文件到所有模块）。

测试：

- 向量加减、点积、叉积、归一化。
- 矩阵乘法和逆。
- Rectangle contains/intersection。

禁止：

- 混用 degree/radian 作为内部单位。

## T02 WGS84 Ellipsoid

目标：

- 实现 `Ellipsoid::WGS84()`。
- 实现 cartographic/ECEF 双向转换。

测试：

- 赤道、本初子午线、极区、高度非 0。

## T03 Camera 与 Pick Ray

目标：

- 实现 Camera state、view/projection matrix、screen pick ray。

测试：

- 不同屏幕分辨率下 screen 到 ray 正确。
- 中心点 ray 指向地球。

## T04 椭球渲染

目标：

- 通过 RenderDevice 渲染可见地球（Metal on iOS、GL ES on Android）。
- 支持相机 orbit/zoom。

验收：

- 首屏渲染 surface 非空白。
- 触控拖动和捏合缩放可用。

## T05 TileScheme XYZ

目标：

- 实现 XYZ Web Mercator TileScheme。

测试：

- z=0 bounds。
- tileToBounds。
- boundsToTileRange。
- y 轴方向。

## T06 Basemap TilePlan

目标：

- 实现 TilePlan 和 LayerTilePlan。
- 实现 visible tile selection 初版。

测试：

- 相机变化后 desiredTiles 变化。
- 不同 layer 共享 visibleTiles 但独立 cache key。

## T07 ImageryProvider

目标：

- 接入标准 XYZ URL provider，通过 PlatformBridge HTTP 发请求。
- 支持 request cancellation。

测试：

- 成功请求。
- 404/timeout 处理。
- 取消后不更新当前帧。

## T08 Texture Upload 与 Tile Cache

目标：

- 平台图片解码（iOS: CGImage、Android: BitmapFactory）后上传 GPU texture。
- raw/decoded/texture cache 分层。

验收：

- 每帧上传数量受控。
- 图层销毁后 GPU 资源释放。

## T09 SurfaceTile Basemap Rendering

目标：

- 实现 `SurfaceTile` 主链路，把底图 imagery 作为 surface attachment 渲染。
- `SurfaceTile` 是地球表面 mesh 和 depth 的唯一来源。
- 支持 parent fallback。

修改模块：

- `tiling/TileSurface`
- `tiling/SurfaceTile`
- `tiling/SurfaceTileMesh`
- `layers/BasemapLayer`
- `renderer/RenderCommand`
- `renderer/Renderer`

核心接口：

- `SurfaceTileKey`
- `SurfaceTile`
- `ImageryAttachment`
- `SurfaceTileCommand`

测试：

- Web Mercator `u/v` 到 WGS84 ECEF 的 surface mesh 测试。
- outward winding 测试。
- parent fallback `uvWindow` 按 Mercator Y 计算。
- `SurfaceTileCommand` 固定 depth/cull/blend 状态验证。
- 背面 surface tile 不进入 render queue。

验收：

- 不白屏。
- debug overlay 显示 z/x/y/state。
- 首屏底图来自 `SurfaceTileCommand`，不是 `GlobeCommand + BasemapTileCommand`。
- 快速旋转和缩放不显示背面或过期 surface tile。

禁止事项：

- 不得把标准底图作为独立共面 imagery mesh 绘制。
- 不得通过关闭 depth test 或 cull face 修复瓦片消失。
- 不得把 Web Mercator tile 的 `v` 线性映射到 geodetic latitude。

## T10 Picking 经纬度

目标：

- 点击地球返回 WGS84 经纬高。

测试：

- center pick 命中。
- sky pick 返回 none。

## T11 Diagnostics

目标：

- 显示 FPS、draw calls、visible tiles、request queue、texture count。

验收：

- debug overlay 可开关。

## T12 GeoJSON VectorLayer

目标：

- 加载 GeoJSON 点线面。
- 基础样式。
- picking feature id。

测试：

- `[lng, lat]` 坐标顺序。
- 点线面位置正确。

## T13 Editing 与 Measurement

目标：

- 绘制点线面。
- 测距/测面。
- undo/redo。

验收：

- Esc（或 back gesture）取消。
- 输出单位和计算模型。

## T14 Terrain MVP

目标：

- 接入 heightmap 或简单 terrain mesh。
- 影像贴地形。

验收：

- 地形和影像对齐。
- picking 返回地形高度。

## T15 稳定性与压力测试

目标：

- 快速缩放、弱网、长时间运行、资源释放测试。
- 移动端专用场景：应用后台/前台切换、内存压力信号、设备旋转。

验收：

- 无明显资源泄漏。
- 过期请求不污染当前帧。
- GL context 丢失后可恢复。
- 应用挂起到后台并返回后引擎状态正常。

## 任务执行规则

- 每个任务完成后必须保持 iOS 和 Android 示例可运行。
- 每个任务必须有测试或可复现验收。
- 不得跨多个阶段一次性大改。
- 遇到架构不清时先更新契约文档。
- 每个任务必须在 iOS 模拟器和至少一台 Android 设备（或模拟器）上验证。
