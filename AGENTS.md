# earth-md AI 工作规则

本项目把 GIS 工作视为“基于证据的工程判断”，不能只靠一句角色提示词或 AI 记忆来做空间逻辑决策。

当任务涉及地图、坐标、投影、CRS、GeoJSON、瓦片、地形、栅格、矢量、轨迹、距离、面积、缓冲区、空间查询、行政区划、地理编码、路径规划或空间数据库时，AI 在回答或改代码前，必须先阅读本项目 GIS 可信知识库。

## 必读路径

1. `docs/gis/README.md`
2. `docs/gis/trusted-sources.md`
3. `docs/gis/project-conventions.md`
4. `docs/gis/spatial-calculation.md`
5. `docs/gis/verification-checklist.md`

## 执行规则

- 如果官方标准、官方库文档、项目约定或本地测试能回答问题，AI 不得只凭记忆编造 GIS 结论。
- 当项目约定不完整时，必须明确说明“这是一个假设”。
- 如果代码和知识库冲突，必须指出冲突，并通过数据流、测试或用户确认来验证，不得直接改行为。
- 坐标转换、几何运算、拓扑关系、空间索引、测地线计算等专业能力，优先使用成熟 GIS 库。
- 每个 GIS 实现都应在代码、测试或文档中明确坐标顺序、CRS、单位和计算模型。
