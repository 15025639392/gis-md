# GIS 可信知识库

这个目录是 `earth-md` 项目本地的 GIS 可信知识库。它的目标是让 AI 助手和人类开发者在处理空间逻辑时，不依赖模糊提示词、不稳定记忆或临时猜测。

只要工作涉及以下内容，就必须使用本知识库：

- 坐标、CRS、投影、基准面、坐标转换
- GeoJSON、WKT/WKB、矢量数据、栅格、DEM、瓦片、地形
- 距离、面积、缓冲区、相交、包含、简化、拓扑
- 空间数据库、空间索引、包围盒、空间查询
- 轨迹、路线、地理编码、行政区划、地图渲染
- 3D 地球、地形 LOD、影像瓦片、3D Tiles、相机交互、GPU 渲染

## 工作流程

1. 先识别 GIS 问题边界：数据格式、CRS、坐标顺序、单位、空间操作和输出结果。
2. 到 `trusted-sources.md` 查对应的权威标准或官方库文档。
3. 到 `project-conventions.md` 查本项目自己的规则。
4. 如果任务涉及地球引擎，先读 `earth-engine-architecture.md`、`earth-coordinate-systems.md`、`tiles-terrain-lod.md`、`multi-tile-schemes.md`、`rendering-engine.md`。
5. 如果任务涉及数据接入、瓦片、样式、环境、3D Tiles、算法、测试或调试，再读对应专题：`data-provider-contracts.md`、`engine-data-catalog.md`、`overlay-styling.md`、`environment-atmosphere-weather.md`、`three-d-tiles.md`、`engine-math-algorithms.md`、`engine-testing-acceptance.md`、`debugging-observability.md`。
6. 用 `spatial-calculation.md` 判断距离、面积、缓冲区、投影和拓扑操作该怎么做。
7. 在完成分析或代码修改前，用 `verification-checklist.md` 和 `engine-development-checklist.md` 做自检。

## 专题地图

- 引擎架构：`earth-engine-architecture.md`
- 坐标系统：`earth-coordinate-systems.md`
- 瓦片、地形、LOD：`tiles-terrain-lod.md`
- 多瓦片体系与无偏移叠加：`multi-tile-schemes.md`
- 渲染管线：`rendering-engine.md`
- 数据 Provider 接口：`data-provider-contracts.md`
- 数据承载目录：`engine-data-catalog.md`
- 数据叠加层样式：`overlay-styling.md`
- 星空、大气、光照、天气：`environment-atmosphere-weather.md`
- 3D Tiles：`three-d-tiles.md`
- 数学与算法：`engine-math-algorithms.md`
- 测试验收：`engine-testing-acceptance.md`
- 调试观测：`debugging-observability.md`

## 证据规则

任何 GIS 结论至少应该有以下一种依据：

- 本目录中的项目约定
- 官方标准或官方库文档
- 本仓库已有代码、测试、fixtures 或样例数据
- 可复现的计算或验证步骤

如果以上依据都没有，必须把结论标记为“假设”，并说明应该如何验证。
