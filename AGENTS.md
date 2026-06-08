# earth-md AI 工作规则

本项目的地球引擎行为以 `/Users/ldy/Desktop/work/openglobus` 为主要对齐对象。

## 执行规则

- 涉及地图、坐标、瓦片、地形、相机、拾取、渲染、LOD、Provider 或 3D globe 行为时，先阅读 `/Users/ldy/Desktop/work/openglobus` 中对应源码，再修改本项目。
- 不再强制读取 `docs/gis/*` 作为开发前置条件。
- 如果本项目既有文档、测试或实现与 OpenGlobus 行为冲突，以 OpenGlobus 源码行为为准。
- 对齐时优先保留 OpenGlobus 的算法结构、状态变量语义、输入单位和边界处理；无法一比一移植时，在代码或测试中标明差异。
- 新增或修改测试时，测试目标应验证 OpenGlobus 行为对齐，而不是验证旧项目约束。
