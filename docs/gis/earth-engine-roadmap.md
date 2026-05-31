# 从 0 开发地球引擎路线图

本文件定义从空项目开发 3D Earth Engine / Globe Engine 的阶段路线。AI 不得跳过基础阶段直接实现复杂能力。每个阶段都要有可运行结果、测试和调试证据。

## 总原则

- 先让地球正确出现，再让数据正确叠加，最后再做复杂效果。
- 先实现可测试的数学和接口，再接入真实网络数据。
- 先单图层、单 provider、单 tile scheme，再扩展多图层、多 provider、多 CRS。
- 先能 debug，再追求视觉精致。
- 每一阶段都必须保持可运行，不允许长期停留在半成品大重构。

## 阶段 0：项目骨架

目标：

- 建立工程脚手架、构建、测试、示例页面。
- 明确渲染后端：WebGL2、WebGPU、Three.js 或自研 renderer。
- 建立模块目录和 public API。

产物：

- `src/core`
- `src/renderer`
- `src/providers`
- `src/layers`
- `src/interaction`
- `src/debug`
- `examples/minimal-globe`
- 单元测试和端到端截图测试入口。

验收：

- 本地能启动空 canvas。
- 测试命令可运行。
- debug 面板能显示基础 frame state。

## 阶段 1：核心数学与坐标

目标：

- 实现 WGS84 ellipsoid。
- 实现 Cartographic、Cartesian3、Matrix4、Ray、Rectangle。
- 实现经纬高到 ECEF、ECEF 到 cartographic、ENU。
- 实现相机基本状态。

产物：

- `Ellipsoid`
- `Cartographic`
- `Cartesian3`
- `Transforms`
- `Ray`
- `Rectangle`

验收：

- 坐标转换有单元测试和误差容差。
- degree/radian 误用能被测试抓住。
- 赤道、极区、高度非 0 的测试通过。

## 阶段 2：可旋转地球

目标：

- 渲染一个 WGS84 椭球体或近似球体。
- 建立相机 orbit、zoom、pan。
- 实现基础 frustum 和 depth 设置。

产物：

- `Globe`
- `Camera`
- `Scene`
- `Renderer`
- `CameraController`

验收：

- 首屏非空白。
- 可拖动旋转、滚轮缩放。
- 相机不穿地。
- 近地和远地不明显抖动。

## 阶段 3：单一 XYZ 底图

目标：

- 接入一个标准 XYZ Web Mercator 影像/地图瓦片 provider。
- 实现 tile scheme、tile bounds、visible tile selection。
- 把瓦片纹理贴到地球上。

产物：

- `TileScheme`
- `ImageryProvider`
- `BasemapLayer`
- `TilePlan`
- `TileCache`

验收：

- 地球表面显示底图。
- debug overlay 显示 z/x/y。
- 快速缩放不大面积白屏。
- 父瓦片 fallback 可用。

## 阶段 4：多图层与多瓦片体系

目标：

- 支持 BasemapLayerStack。
- 支持 XYZ、TMS、WMTS 或至少一个非标准 provider profile。
- 支持 TilePlan 分组与每图层 LayerTilePlan。

产物：

- `CrsProfile`
- `TileGroupKey`
- `LayerTilePlan`
- 图层透明度、顺序、显隐。

验收：

- 两个同 scheme 图层共享 visibleTiles 但独立缓存。
- 不同 scheme 图层分别计算 TilePlan。
- 控制点验收无系统性偏移。

## 阶段 5：矢量叠加与样式

目标：

- 支持 GeoJSON 点、线、面。
- 实现基础样式和 picking。
- 支持 hover/selected 状态。

产物：

- `VectorLayer`
- `OverlayStyle`
- `PickingService`
- `SelectionManager`

验收：

- 点线面位置正确。
- picking 返回 feature id。
- hover/selected 不重建整层。
- 坐标顺序和 CRS 明确。

## 阶段 6：地形

目标：

- 接入 heightmap 或 quantized mesh。
- 影像贴地形。
- 处理 skirt、接缝和地形拾取。

产物：

- `TerrainProvider`
- `TerrainTile`
- `TerrainMesh`
- `TerrainLayer`

验收：

- 地形起伏可见。
- 影像和地形对齐。
- 接缝不明显。
- 地形拾取返回高度。

## 阶段 7：绘制、测量与编辑

目标：

- 支持点线面绘制。
- 支持距离、面积、高程测量。
- 支持撤销/重做。

产物：

- `EditingManager`
- `MeasurementTool`
- `CommandBus`
- `AnnotationLayer`

验收：

- Esc 可取消。
- undo/redo 可用。
- 测量显示单位和计算模型。

## 阶段 8：3D Tiles 与模型

目标：

- 支持 tileset traversal。
- 支持 geometricError、SSE、content 生命周期。
- 支持 glTF 模型。

产物：

- `TilesetProvider`
- `TilesetLayer`
- `TileTraversal`
- `ModelLayer`

验收：

- tileset 渐进加载。
- picking 返回 feature 或 object id。
- 卸载 tile 释放 GPU 资源。

## 阶段 9：环境系统

目标：

- 支持天空、星空、大气、太阳光照、昼夜线。
- 可选支持天气/云/雾的 visual-only 效果。

产物：

- `EnvironmentSystem`
- `TimeController`
- `LightingProvider`
- `Atmosphere`

验收：

- 白天/夜晚切换合理。
- 环境效果可关闭。
- 不污染业务图层颜色和分析数据。

## 阶段 10：性能、离线与工程化

目标：

- 完善缓存、worker、资源释放、profiling。
- 支持离线包或本地数据。
- 完善文档、示例、回归测试。

验收：

- 有 FPS、draw call、tile queue、GPU resource 观测。
- 快速操作不泄漏资源。
- 关键场景有自动化截图或回归测试。

## 禁止跳跃

- 不得在没有坐标测试的情况下接入多 CRS 图层。
- 不得在没有 debug overlay 的情况下实现复杂瓦片调度。
- 不得在没有资源释放策略的情况下接入 3D Tiles。
- 不得在没有 picking 契约的情况下实现编辑工具。
- 不得把 visual-only 天气当作 analytic 数据能力。
