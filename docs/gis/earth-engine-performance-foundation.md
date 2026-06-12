# 地球引擎高性能基座契约

本文定义 earth-md 地球引擎的长期性能基座。它不是单次优化清单，而是所有瓦片、地形、渲染、交互、Provider、上传和环境效果都必须遵守的工程契约。

目标：在移动端优先保证 60 FPS 的交互跟手性，在不牺牲空间正确性、影像清晰度和可诊断性的前提下持续优化。

长期架构边界见 `earth-engine-final-streaming-architecture.md`。所有性能改动应先判断属于主线程策略线、后台异步线还是 GPU/渲染层线，避免把可后台化或可批处理的工作继续堆在主线程。

## 基本原则

- 性能不能靠降低地图清晰度获得，尤其不能把临时放宽 SSE 当作主策略。
- 性能问题必须能被指标定位到网络、解析、调度、上传、tile plan、draw/state、fragment、内存或同步等待中的某一层。
- 主路径必须简单、稳定、可预测；复杂视觉效果放在独立 pass 或 variant，不污染最高频路径。
- 所有异步结果都必须可取消、可丢弃、可判定过期。
- 所有 GPU 资源都必须有所有权、预算、释放和 context/device lost 恢复策略。
- 优化不能掩盖 tile selection、LOD、provider、地形、坐标或上传生命周期缺陷。

## 性能预算

初始预算按 Android 物理机和 iOS 中端设备设计，后续用实测更新。

| 项目 | 目标 | 说明 |
| --- | --- | --- |
| 交互 FPS | 60 FPS | 高倾斜、拖拽、缩放时优先保证 |
| 单帧 CPU update | <= 4 ms | 不含后台线程 |
| 单帧 render submit | <= 2 ms | GL/Metal command 构造和状态提交 |
| 单帧 GPU upload | <= 1-2 ms | 交互帧内节流，空闲帧可放宽 |
| SurfaceTile draw calls | 可解释且稳定 | 不能随交互抖动异常翻倍 |
| Shader program switch | 按 variant 分桶 | 不允许 tile 间无序切换 |
| Texture upload count | 有帧预算 | 不允许请求完成后同帧集中上传 |
| Tile plan rebuild | 每帧至多必要一次 | upload 完成不应立即触发重复全量 rebuild |
| GPU texture cache | 有总量和 per-layer 上限 | 必须可观测和可淘汰 |

任何指标超过预算，都要先判断是常态成本还是尖峰成本。尖峰优先处理调度、上传、重建和同步等待。

## 主路径定义

高频主路径指默认底图、无透明图层、无 debug overlay、无复杂后处理时的渲染路径：

```text
Input update
Tile plan / cache handoff
SurfaceTileOpaqueNoWater, front-to-back
SurfaceTileOpaqueWater, front-to-back
Atmosphere/ground composite
Overlay/UI if enabled
```

主路径要求：

- SurfaceTile opaque 路径不启用 blend。
- 无 water mask 的 tile 不执行 water branch，不绑定 water sampler。
- 远距雾、地平线空气、天空过渡不在 surface tile fragment shader 中实现。
- transition 不通过整 tile alpha 淡到天空背景。
- debug overlay、vector overlay、normal map debug 不纳入默认性能基准。

## 模块职责

### Tile Selection

负责：

- 根据相机、视锥、horizon culling、SSE 和 provider 可用性选择必要 tile。
- 保持清晰度和稳定 handoff。
- 避免父子 tile 无预算同画。

不负责：

- 用降低 LOD 遮盖 fragment 或 upload 压力。
- 用 tile 选择解决大气合成问题。

### Surface Rendering

负责：

- 影像采样。
- water mask variant。
- layer opacity。
- 必要 terrain/material lighting。
- depth write。

不负责：

- 远距大气雾。
- 地平线天空过渡。
- provider 纠偏。
- LOD transition 遮丑。

### Atmosphere/Ground Compositing

负责：

- 读取 depth/color，对 sky pixel 和 ground pixel 统一处理空气透视。
- 处理地平线、低空雾、天空大气散射和地表远距衰减。
- 保持 surface tile shader 简单。

### Upload Scheduler

负责：

- request completion、decode、GPU upload、renderable handoff 分阶段。
- 每帧 upload 数量和耗时预算。
- 过期任务清理。
- 避免交互帧集中创建 texture/buffer。

### Renderer Backend

负责：

- 按 variant 和状态分桶。
- 缓存 uniform location、vertex layout、pipeline state。
- 跟踪 draw/state/texture/program 切换指标。
- 不在每帧重复创建 shader、buffer、texture 或 framebuffer。

## Shader Variant 基线

必须至少支持：

- `SurfaceTileOpaqueNoWater`
- `SurfaceTileOpaqueWater`
- `SurfaceTileLayerBlendNoWater`
- `SurfaceTileLayerBlendWater`

后续 terrain/material、debug、picking 可以继续扩展 variant，但不得把所有逻辑塞回一个万能 shader。

variant 引入时必须记录：

- 触发条件。
- 是否写 depth。
- 是否 blend。
- texture sampler 数量。
- fragment 中是否有 branch、discard、额外 texture sample。
- 目标设备上的 A/B 指标。

## 固定 Benchmark 场景

所有基础性能改动必须至少覆盖：

- Normal nadir：常规俯视。
- Near-ground high tilt：低空高倾斜。
- Mid-altitude oblique：中高度斜视。
- Horizon/limb：地平线和大气边缘。
- Fast interaction：快速拖拽、缩放、倾斜。
- Upload stress：弱网或大量 tile 返回。

每个场景记录：

- screenshot。
- FPS。
- update ms。
- render submit ms。
- draw calls。
- visible tile / renderSurface tile。
- shader variant 分布。
- texture upload count/time。
- tile plan rebuild count/time。
- GPU texture cache size。
- glError 或 backend error。

没有固定 benchmark 的性能结论只能称为假设。

## 禁止项

- 为了性能默认放宽 SSE 或降低清晰度。
- 为了视觉遮盖在 surface tile fragment 中堆复杂大气/雾。
- 每帧创建 GPU 资源。
- 同一帧多次全量 rebuild tile plan。
- 过期请求覆盖当前 generation。
- 透明 transition 打断 opaque 主路径。
- 父子 tile 无预算同屏重复画。
- 用 debug overlay 或后处理遮盖 tile 缝、缺图、LOD 抖动。
- 只看 FPS，不记录 draw/upload/plan/cache 指标。

## 发布门禁

合入高性能基座相关改动前必须回答：

- 主路径是否更简单，还是引入了新的 per-fragment/per-tile 成本？
- 是否影响 SSE、LOD 或清晰度？
- 是否增加 draw call、program switch、texture bind 或 upload 峰值？
- 是否有固定视角 A/B 截图和指标？
- 是否有 Android 实机验证或等价后端验证？
- 是否保留可诊断日志或性能计数器？
- 是否会影响 provider、tile cache、terrain 或 vector overlay 的生命周期？

如果无法回答，改动只能作为实验分支，不能成为默认基座。

## 实施顺序

1. 固化 benchmark 入口和性能计数器。
2. 拆 SurfaceTile shader variants。
3. 移除 surface tile 中的远距雾主逻辑。
4. 分桶 draw/state，稳定 opaque front-to-back。
5. 增加 upload budget 与 handoff trace。
6. 建立 offscreen color/depth 管线。
7. 实现 depth-aware atmosphere/ground compositing。
8. 增加长期性能回归脚本和截图归档。

## 相关文档

- `earth-engine-final-streaming-architecture.md`
- `performance-data-stability.md`
- `graphics-pipeline.md`
- `rendering-engine.md`
- `basemap-tile-rendering.md`
- `surface-tile-mainline.md`
- `high-tilt-performance-atmosphere.md`
