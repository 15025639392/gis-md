# earth-md AI 工作规则

本项目把 GIS 工作视为“基于证据的工程判断”，不能只靠一句角色提示词或 AI 记忆来做空间逻辑决策。

当任务涉及地图、坐标、投影、CRS、GeoJSON、瓦片、地形、栅格、矢量、轨迹、距离、面积、缓冲区、空间查询、行政区划、地理编码、路径规划、空间数据库、3D 地球、globe rendering、terrain LOD、imagery layer、3D Tiles、相机控制或 GPU 渲染时，AI 在回答或改代码前，必须先阅读本项目 GIS/地球引擎可信知识库。

## 必读路径

1. `docs/gis/README.md`
2. `docs/gis/trusted-sources.md`
3. `docs/gis/project-conventions.md`
4. `docs/gis/earth-engine-architecture.md`
5. `docs/gis/earth-coordinate-systems.md`
6. `docs/gis/tiles-terrain-lod.md`
7. `docs/gis/rendering-engine.md`
8. `docs/gis/data-provider-contracts.md`
9. `docs/gis/three-d-tiles.md`
10. `docs/gis/engine-math-algorithms.md`
11. `docs/gis/engine-testing-acceptance.md`
12. `docs/gis/debugging-observability.md`
13. `docs/gis/spatial-calculation.md`
14. `docs/gis/verification-checklist.md`
15. `docs/gis/engine-development-checklist.md`

## 执行规则

- 如果官方标准、官方库文档、项目约定或本地测试能回答问题，AI 不得只凭记忆编造 GIS 结论。
- 当项目约定不完整时，必须明确说明“这是一个假设”。
- 如果代码和知识库冲突，必须指出冲突，并通过数据流、测试或用户确认来验证，不得直接改行为。
- 坐标转换、几何运算、拓扑关系、空间索引、测地线计算等专业能力，优先使用成熟 GIS 库。
- 每个 GIS 实现都应在代码、测试或文档中明确坐标顺序、CRS、单位和计算模型。
- 地球引擎相关实现必须额外明确：参考椭球体、世界坐标系、局部坐标系、相机模型、瓦片方案、LOD 策略、精度策略和性能预算。
- 数据接入、瓦片调度、3D Tiles、渲染、相机和拾取功能必须有可测试接口契约，不得只靠视觉观察判断正确。
