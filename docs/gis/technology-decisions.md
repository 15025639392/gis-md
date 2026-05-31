# 技术栈决策

从 0 开发地球引擎前，必须先记录技术栈决策。没有技术路线，AI 很容易在 Three.js、自研 WebGL、WebGPU、业务框架之间摇摆，导致架构混乱。

## 推荐默认路线

如果项目没有额外约束，推荐 MVP 默认路线：

- 语言：TypeScript。
- 目标平台：Web browser。
- 构建：Vite 或等价轻量构建。
- 测试：Vitest 单元测试 + Playwright 视觉/交互验收。
- 渲染后端：先 WebGL2，Renderer 接口预留 WebGPU 后端。
- MVP 模式：只做 3D globe，不做 2D/Columbus view。
- 数学：核心向量/矩阵可自研小集合，但 CRS 转换不手写复杂投影。
- 图层：先 XYZ Web Mercator 底图，再扩展其他 provider。

这个默认路线的目标是降低变量数量，让坐标、瓦片、相机和渲染主链路先跑通。

## 必填决策

每个从 0 开发任务必须填写：

- 目标平台：Web、Native、Mobile，还是多端。
- 语言：TypeScript、Rust、C++、Kotlin、Swift 等。
- 渲染后端：Three.js、custom WebGL2、WebGPU、Native GPU。
- 数学库：自研、gl-matrix、three math，还是其他。
- 测试框架。
- 示例应用框架。
- 是否支持 worker。
- 是否支持离线包。
- 是否支持移动端作为 MVP 目标。

## Three.js 路线

适合：

- 快速 MVP。
- 借用成熟 scene graph、材质、模型加载。
- 团队图形底层经验有限。

风险：

- 地球尺度精度仍需自己处理。
- TilePlan、Provider、CRS、数据调度不能交给 Three.js。
- 深度、透明、picking 和资源释放仍需严格约束。

## 自研 WebGL2 路线

适合：

- 想掌控渲染管线。
- 目标是长期引擎内核。
- 能投入图形基础设施。

风险：

- 初期开发慢。
- shader、buffer、texture、framebuffer、context lost 都要自己处理。
- 需要更强测试和 debug overlay。

## WebGPU 路线

适合：

- 长期面向现代浏览器。
- 需要 compute、storage buffer 或更现代 GPU 管线。

风险：

- 兼容性和生态仍需确认。
- MVP 不应同时承担 WebGPU 学习成本和 GIS 引擎复杂度。

## 决策记录模板

```text
Decision:
  date:
  owner:
  choice:
  alternatives:
  reason:
  consequences:
  revisit condition:
```

重大技术决策必须可追溯，不要只散落在聊天记录里。
