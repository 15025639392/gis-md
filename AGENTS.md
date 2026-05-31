# earth-md AI 工作规则

本项目把 GIS 工作视为“基于证据的工程判断”，不能只靠一句角色提示词或 AI 记忆来做空间逻辑决策。

当任务涉及地图、坐标、投影、CRS、GeoJSON、瓦片、地形、栅格、矢量、轨迹、距离、面积、缓冲区、空间查询、行政区划、地理编码、路径规划、空间数据库、3D 地球、globe rendering、terrain LOD、imagery layer、3D Tiles、相机控制或 GPU 渲染时，AI 在回答或改代码前，必须先阅读本项目 GIS/地球引擎可信知识库。

## 必读路径

1. `docs/gis/README.md`
2. `docs/gis/trusted-sources.md`
3. `docs/gis/project-conventions.md`
4. `docs/gis/earth-engine-architecture.md`
5. `docs/gis/earth-engine-roadmap.md`
6. `docs/gis/reference-architecture.md`
7. `docs/gis/core-api-contracts.md`
8. `docs/gis/mvp-acceptance.md`
9. `docs/gis/technology-decisions.md`
10. `docs/gis/task-breakdown.md`
11. `docs/gis/test-fixtures.md`
12. `docs/gis/provider-compatibility-matrix.md`
13. `docs/gis/maturity-model.md`
14. `docs/gis/error-recovery.md`
15. `docs/gis/earth-coordinate-systems.md`
16. `docs/gis/tiles-terrain-lod.md`
17. `docs/gis/multi-tile-schemes.md`
18. `docs/gis/basemap-tile-rendering.md`
19. `docs/gis/rendering-engine.md`
20. `docs/gis/graphics-pipeline.md`
21. `docs/gis/data-provider-contracts.md`
22. `docs/gis/engine-data-catalog.md`
23. `docs/gis/overlay-styling.md`
24. `docs/gis/environment-atmosphere-weather.md`
25. `docs/gis/interaction-system.md`
26. `docs/gis/three-d-tiles.md`
27. `docs/gis/engine-math-algorithms.md`
28. `docs/gis/algorithm-implementation-details.md`
29. `docs/gis/performance-data-stability.md`
30. `docs/gis/engine-testing-acceptance.md`
31. `docs/gis/debugging-observability.md`
32. `docs/gis/spatial-calculation.md`
33. `docs/gis/verification-checklist.md`
34. `docs/gis/engine-development-checklist.md`

## 执行规则

- 如果官方标准、官方库文档、项目约定或本地测试能回答问题，AI 不得只凭记忆编造 GIS 结论。
- 当项目约定不完整时，必须明确说明“这是一个假设”。
- 如果代码和知识库冲突，必须指出冲突，并通过数据流、测试或用户确认来验证，不得直接改行为。
- 坐标转换、几何运算、拓扑关系、空间索引、测地线计算等专业能力，优先使用成熟 GIS 库。
- 每个 GIS 实现都应在代码、测试或文档中明确坐标顺序、CRS、单位和计算模型。
- 地球引擎相关实现必须额外明确：参考椭球体、世界坐标系、局部坐标系、相机模型、瓦片方案、LOD 策略、精度策略和性能预算。
- 数据接入、瓦片调度、3D Tiles、渲染、相机和拾取功能必须有可测试接口契约，不得只靠视觉观察判断正确。
- 新增任何地球引擎数据类型时，必须先在 `engine-data-catalog.md` 中明确分类、坐标、单位、时间维度、LOD、样式、权限、缓存和验收方式。
- 多瓦片体系叠加必须通过 `multi-tile-schemes.md` 定义的 TileScheme、CRS profile、坐标转换和控制点验收来证明无系统性偏移，不能只靠“看起来差不多”。
- 作为地球底图的影像/地图瓦片渲染必须遵守 `basemap-tile-rendering.md`，明确可见性选择、请求调度、缓存、纹理上传、父子替换、多图层混合和失败降级策略。
- 点、线、面、标注、模型、点云和分析结果的样式必须遵守 `overlay-styling.md`，把数据 schema、样式表达式、交互状态、LOD、贴地/高度和性能预算分开设计。
- 星空、大气、太阳/月亮、光照、阴影、云雾雨雪、海洋和时间系统必须遵守 `environment-atmosphere-weather.md`，明确真实物理、近似渲染和纯视觉效果的边界。
- 相机、手势、拾取、选择、绘制、编辑、测量、时间轴和图层控制必须遵守 `interaction-system.md`，把输入事件、空间结果、状态机和撤销/重做分开设计。
- 图形渲染、shader、材质、深度、透明、后处理、GPU 资源和性能优化必须遵守 `graphics-pipeline.md`，不得用临时视觉 hack 掩盖坐标或数据错误。
- 从 0 开发地球引擎时，必须按 `earth-engine-roadmap.md` 分阶段推进，按 `reference-architecture.md` 组织目录和模块，按 `core-api-contracts.md` 定义接口，并用 `mvp-acceptance.md` 验收最小可运行版本。
- 从 0 开发前必须先记录 `technology-decisions.md` 中的技术栈决策，并按 `task-breakdown.md` 拆成可验证任务；不得在技术路线未定时混用 Three.js、自研 WebGL/WebGPU 和业务框架假设。
- 性能优化必须优先检查 `performance-data-stability.md` 中的数据约束、预处理、索引、LOD、缓存、worker 和底层正确性门禁；不得用渲染层 hack 掩盖数据过载或核心实现不稳定。
- 核心算法实现必须遵守 `algorithm-implementation-details.md`，每个算法都要明确输入输出、坐标空间、误差模型、边界情况、测试样例和调试观测。
- 能力成熟度必须按 `maturity-model.md` 标注；Provider 接入必须更新 `provider-compatibility-matrix.md`；错误处理和恢复必须遵守 `error-recovery.md`；测试数据必须优先使用 `test-fixtures.md`。
