# 地球引擎架构规则

本文件用于指导 AI 设计和实现 3D Earth Engine / Globe Engine。地球引擎不是普通地图组件，它同时涉及 GIS、图形学、数据调度、数值精度和交互体验。

## 必须先明确的问题

在设计任何地球引擎功能前，先回答：

- 目标是 2D map、2.5D terrain，还是 3D globe？
- 使用的参考椭球体是什么，默认是否为 WGS84 ellipsoid？
- 世界坐标系是什么：ECEF、局部 ENU、Web Mercator 平面，还是渲染引擎自定义坐标？
- 输入数据类型是什么：imagery tile、terrain tile、vector tile、GeoJSON、3D Tiles、glTF、raster DEM？
- 数据调度单位是什么：tile、chunk、layer、primitive、entity，还是 scene graph node？
- 可见性和 LOD 由什么驱动：相机距离、screen-space error、zoom level、geometric error，还是固定层级？
- 主要瓶颈是什么：网络、CPU 解析、GPU buffer、纹理显存、draw call、shader、精度，还是内存回收？

## 推荐模块边界

地球引擎应尽量拆成清晰层次：

- `Core Geodesy`：椭球体、经纬高、ECEF、ENU、投影、坐标转换。
- `Tile Scheme`：XYZ、TMS、quadtree、geographic tiling、Web Mercator tiling。
- `Data Providers`：影像、地形、矢量、3D Tiles、离线数据、缓存。
- `Scene Graph / Primitive`：地球、地形、瓦片网格、矢量覆盖物、模型。
- `Renderer`：shader、buffer、texture、material、picking、depth、culling。
- `Camera Controller`：orbit、pan、zoom、tilt、fly-to、collision、地表约束。
- `Scheduler`：瓦片优先级、请求并发、取消、缓存淘汰、渐进加载。
- `Validation / Diagnostics`：坐标、瓦片、帧率、显存、请求、LOD、误差可视化。

## AI 开发原则

- 不要把地球引擎写成一个巨大的 `MapView` 或 `Globe` 类。
- 不要让渲染代码直接散落处理 CRS、网络请求和业务状态。
- 所有坐标转换必须集中在 geodesy/core 层。
- 所有 tile id、tile bounds、tile matrix、LOD 逻辑必须可测试。
- 所有 GPU 资源必须有生命周期：创建、复用、释放、上下文丢失恢复。
- 异步加载必须支持取消，避免相机快速移动后旧瓦片覆盖新状态。

## 最小可运行地球引擎顺序

1. 实现 WGS84 ellipsoid 和经纬高到 ECEF 的转换。
2. 建立相机模型和视锥 culling。
3. 渲染一个可旋转的椭球体或球体。
4. 接入单一影像瓦片图层。
5. 实现 quadtree tile selection 和 LOD。
6. 接入地形瓦片并处理 skirt、法线、边界缝。
7. 添加 picking、标注、矢量覆盖物。
8. 再扩展 3D Tiles、模型、离线缓存和高级分析。

## 不能省略的工程证据

地球引擎相关 PR 或 AI 修改必须说明：

- 坐标系和单位
- LOD 选择依据
- 边界情况：极区、反经线、地平线、相机穿地、瓦片接缝
- 性能影响：请求数、draw call、纹理数量、buffer 数量、内存
- 验证方式：单元测试、视觉截图、调试 overlay、性能指标
