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

- 建立 TypeScript/Web MVP 项目骨架。
- 提供示例页面、测试命令、lint/format 可选。

验收：

- `npm install` 后能启动示例。
- 空 canvas 可显示。
- 单元测试命令可运行。

## T01 基础数学类型

目标：

- 实现 `Cartesian3`、`Cartographic`、`Matrix4`、`Ray`、`Rectangle`。

测试：

- 向量加减、点积、叉积、归一化。
- 矩阵乘法和逆矩阵。
- Rectangle contains/intersection。

禁止：

- 混用 degree/radian。

## T02 WGS84 Ellipsoid

目标：

- 实现 `Ellipsoid.WGS84`。
- 实现 cartographic/ECEF 双向转换。

测试：

- 赤道、本初子午线、极区、高度非 0。

## T03 Camera 与 Pick Ray

目标：

- 实现 Camera state、view/projection matrix、screen pick ray。

测试：

- DPR 不同情况下 screen 到 ray 正确。
- 中心点 ray 指向地球。

## T04 椭球渲染

目标：

- 渲染可见地球。
- 支持相机 orbit/zoom。

验收：

- 首屏非空白。
- 拖动和滚轮可用。

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

- 接入标准 XYZ URL provider。
- 支持 request cancellation。

测试：

- 成功请求。
- 404/timeout 处理。
- 取消后不更新当前帧。

## T08 Texture Upload 与 Tile Cache

目标：

- 图片解码和 GPU texture 上传。
- raw/decoded/texture cache 分层。

验收：

- 每帧上传数量受控。
- 图层销毁后资源释放。

## T09 Basemap Rendering

目标：

- 把底图瓦片贴到地球表面。
- 支持 parent fallback。

验收：

- 不白屏。
- debug overlay 显示 z/x/y/state。

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

- Esc 取消。
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

验收：

- 无明显资源泄漏。
- 过期请求不污染当前帧。

## 任务执行规则

- 每个任务完成后必须保持示例可运行。
- 每个任务必须有测试或可复现验收。
- 不得跨多个阶段一次性大改。
- 遇到架构不清时先更新契约文档。
