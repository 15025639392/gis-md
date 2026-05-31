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
4. 如果任务涉及地球引擎，先读 `earth-engine-architecture.md`、`earth-coordinate-systems.md`、`tiles-terrain-lod.md`、`rendering-engine.md`。
5. 用 `spatial-calculation.md` 判断距离、面积、缓冲区、投影和拓扑操作该怎么做。
6. 在完成分析或代码修改前，用 `verification-checklist.md` 做自检。

## 证据规则

任何 GIS 结论至少应该有以下一种依据：

- 本目录中的项目约定
- 官方标准或官方库文档
- 本仓库已有代码、测试、fixtures 或样例数据
- 可复现的计算或验证步骤

如果以上依据都没有，必须把结论标记为“假设”，并说明应该如何验证。
