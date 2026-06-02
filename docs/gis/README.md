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
4. 如果任务涉及从 0 开发或重构地球引擎，先读 `earth-engine-roadmap.md`、`technology-decisions.md`、`reference-architecture.md`、`core-api-contracts.md`、`task-breakdown.md`、`mvp-acceptance.md`、`implementation-prompts.md`。
5. 如果任务涉及地球引擎模块，读 `earth-engine-architecture.md`、`earth-coordinate-systems.md`、`tiles-terrain-lod.md`、`multi-tile-schemes.md`、`rendering-engine.md`。
6. 如果任务涉及数据接入、瓦片、底图渲染、样式、环境、交互、图形渲染、3D Tiles、算法、性能、测试或调试，再读对应专题：`data-provider-contracts.md`、`engine-data-catalog.md`、`basemap-tile-rendering.md`、`overlay-styling.md`、`environment-atmosphere-weather.md`、`interaction-system.md`、`graphics-pipeline.md`、`three-d-tiles.md`、`engine-math-algorithms.md`、`algorithm-implementation-details.md`、`performance-data-stability.md`、`engine-testing-acceptance.md`、`debugging-observability.md`。
7. 如果任务涉及多线程、异步任务或 GPU 上传线程边界，读 `threading-architecture.md`。
8. 如果任务涉及网络请求、用户数据、文件 IO 或 API key 管理，读 `security.md`。
9. 如果任务涉及离线缓存、数据包导入或后台下载，读 `offline-and-packaging.md`。
10. 在开始编码前，读 `implementation-prompts.md` 获取对应阶段的 AI 开发提示词，读 `common-pitfalls.md` 避免已知错误，读 `ai-workflow-prompt.md` 获取完整 GIS 专家工作流提示词。
11. 用 `spatial-calculation.md` 判断距离、面积、缓冲区、投影和拓扑操作该怎么做。
12. 在完成分析或代码修改前，用 `verification-checklist.md` 和 `engine-development-checklist.md` 做自检。

## 专题地图

- 引擎架构：`earth-engine-architecture.md`
- 从 0 开发路线图：`earth-engine-roadmap.md`
- 参考架构：`reference-architecture.md`
- 核心 API 契约：`core-api-contracts.md`
- 技术栈决策：`technology-decisions.md`
- 任务级拆解：`task-breakdown.md`
- 测试 fixtures：`test-fixtures.md`
- Provider 兼容矩阵：`provider-compatibility-matrix.md`
- 成熟度模型：`maturity-model.md`
- 错误恢复：`error-recovery.md`
- MVP 验收：`mvp-acceptance.md`
- 分阶段开发提示词：`implementation-prompts.md`
- 坐标系统：`earth-coordinate-systems.md`
- 瓦片、地形、LOD：`tiles-terrain-lod.md`
- 多瓦片体系与无偏移叠加：`multi-tile-schemes.md`
- 底图瓦片渲染编排：`basemap-tile-rendering.md`
- 渲染管线：`rendering-engine.md`
- 图形学与 GPU 管线：`graphics-pipeline.md`
- 渲染重点审查：`rendering-review-checklist.md`
- 数据 Provider 接口：`data-provider-contracts.md`
- 数据承载目录：`engine-data-catalog.md`
- 数据叠加层样式：`overlay-styling.md`
- 星空、大气、光照、天气：`environment-atmosphere-weather.md`
- 交互系统：`interaction-system.md`
- 3D Tiles：`three-d-tiles.md`
- 数学与算法：`engine-math-algorithms.md`
- 算法实现细节：`algorithm-implementation-details.md`
- 性能、数据约束与稳定性：`performance-data-stability.md`
- 测试验收：`engine-testing-acceptance.md`
- 调试观测：`debugging-observability.md`
- AI 专家工作流提示词：`ai-workflow-prompt.md`
- 常见 GIS 错误：`common-pitfalls.md`
- 线程架构：`threading-architecture.md`
- 安全架构：`security.md`
- 离线与打包：`offline-and-packaging.md`
- Shader 接口规范：`shader-interface.md`
- Shader 编译管线：`shader-compilation.md`
- 构建与 CI：`build-and-ci.md`
- 部署与分发：`deployment.md`
- 性能剖析：`profiling-guide.md`

## 证据规则

任何 GIS 结论至少应该有以下一种依据：

- 本目录中的项目约定
- 官方标准或官方库文档
- 本仓库已有代码、测试、fixtures 或样例数据
- 可复现的计算或验证步骤

如果以上依据都没有，必须把结论标记为“假设”，并说明应该如何验证。
