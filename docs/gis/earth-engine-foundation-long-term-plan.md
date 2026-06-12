# 地球引擎基座长期打磨计划

地球引擎基座需要长期迭代。它不是一次 shader 优化、一次 tile traversal 调整或一次大气效果修正能完成的工作，而是围绕可测、可复现、可回归的核心链路持续收敛。

本文定义长期工作节奏、阶段目标和每阶段必须产出的证据。硬性性能契约见 `earth-engine-performance-foundation.md`。

## 长期目标

- 高倾斜、近地、快速交互时保持跟手。
- 不用降低 SSE、降低 LOD 或洗白地表换取 FPS。
- SurfaceTile 主路径长期保持简单。
- tile selection、upload、render、atmosphere、interaction 的职责清晰。
- 每次优化都有固定视角 A/B、指标和截图。
- 每个性能问题都能定位到具体成本中心。

## 工作节奏

每轮基座打磨按固定循环进行：

1. 选定一个固定场景或症状。
2. 采集 baseline：截图、FPS、draw、tile、upload、plan、submit、cache。
3. 提出一个最小假设。
4. 做最小改动或最小诊断实验。
5. 用同一视角 A/B 验证。
6. 保留或回滚。
7. 更新文档、指标或回归用例。

没有 baseline 的优化只能算实验，不能算基座改进。

## 阶段 1：固定 Benchmark 与观测能力

目标：

- 让性能问题不再靠肉眼和手感猜。
- 为高倾斜、上传、交互和视觉过渡建立固定复现入口。

产物：

- Android MinimalGlobe 固定 QA 视角。
- 每帧或采样帧性能指标。
- 截图归档目录。
- 可比较的 A/B 输出格式。

必须记录：

- FPS。
- draw calls。
- visible tiles / renderSurface tiles。
- shader variant 分布。
- texture upload count/time。
- tile plan rebuild count/time。
- render submit time。
- GPU texture cache size。
- `glError` 或后端错误。

完成标准：

- 任一基座改动都能在固定视角下复现并比较。
- 高倾斜视角不再依赖手势手动调到“差不多”。

## 阶段 2：SurfaceTile 主路径瘦身

目标：

- 把最高频的 surface tile fragment 和 draw/state 成本降到可控。

工作项：

- 拆分 `SurfaceTileOpaqueNoWater` 与 `SurfaceTileOpaqueWater`。
- 拆分 opaque 与 layer blend 路径。
- 移除 surface tile shader 中的远距雾主逻辑。
- 保留 opaque front-to-back。
- 按 shader variant、texture、layer 分桶。

完成标准：

- 默认底图主路径主要走 `SurfaceTileOpaqueNoWater`。
- 无 water mask tile 没有 water sampler、water branch 或第二纹理采样。
- surface tile shader 不承担地平线大气过渡职责。

## 阶段 3：Upload、Transition 与 Handoff 稳定

目标：

- 消除交互帧 upload/rebuild 尖峰。
- 让 parent fallback 到 child exact 的切换稳定且不引入整 tile alpha 混合。

工作项：

- request completion、decode、GPU upload、renderable handoff 分阶段 trace。
- 每帧 upload budget。
- 避免 upload 后同帧重复全量 rebuild。
- 清理过期 requested/in-flight 状态。
- 检查父子 tile 是否无预算同屏重复绘制。

完成标准：

- 快速交互时没有明显 upload/rebuild 尖峰。
- tile handoff 不导致地球淡到天空或大面积透明。
- 弱网或大量请求返回时仍保持可交互。

## 阶段 4：Depth-Aware Atmosphere/Ground Compositing

目标：

- 把雾、大气、地平线过渡从 surface tile shader 迁出。
- 统一 sky pixel 与 ground pixel 的空气透视模型。

工作项：

- 建立 offscreen color/depth 管线。
- 支持 depth texture 或可采样 depth attachment。
- fullscreen composite pass 读取 color/depth。
- 对 ground pixel 按 ray distance/相机高度/太阳方向做空气透视。
- 对 sky pixel 做天空大气散射。
- 删除临时 horizon haze workaround。

完成标准：

- 高倾斜地平线没有硬蓝边、浅色 tile 带或地表洗白。
- SurfaceTile fragment 成本不随大气效果增加。
- 大气合成可单独关闭、调试和 A/B。

## 阶段 5：Tile Selection 与 Culling 收敛

目标：

- 保持清晰度前提下减少不必要 tile。
- 不把 SSE 放宽作为性能主策略。

工作项：

- 对齐 cesium-native 的关键 tile selection / culling 语义。
- 检查 horizon tangent、frustum、camera-inside、neighbor balance 的真实收益。
- 建立 tile plan 指标：walk/render/frustum/horizon/equal-zoom/balance。
- 高倾斜场景验证 tile 数、清晰度和 FPS。

完成标准：

- tile selection 的每个特殊分支都有指标解释。
- 高倾斜清晰度不回退。
- 可见 tile 数与 draw call 不出现异常倍增。

## 阶段 6：长期回归体系

目标：

- 防止基座性能在后续功能开发中悄悄退化。

工作项：

- 截图回归。
- 固定视角性能阈值。
- shader variant 分布阈值。
- upload spike 阈值。
- draw call / renderSurface tile 阈值。
- Android 实机周期性验证。

完成标准：

- 性能退化能被 CI 或本地脚本尽早发现。
- 新功能必须说明是否影响主路径和预算。

## 每轮打磨的提交说明要求

每轮基座改动至少说明：

- 触碰的成本中心。
- baseline 指标。
- 修改后的指标。
- 同视角截图路径。
- 是否影响 SSE/LOD/清晰度。
- 是否影响 SurfaceTile 主路径。
- 是否增加 shader variant、draw call、texture bind 或 framebuffer pass。
- 验证设备和命令。

## 当前优先级

1. 固化高倾斜 QA 入口和指标采集。
2. 拆 SurfaceTile shader variants。
3. 移除 tile shader 里的远距雾。
4. 加 upload/handoff trace 与预算。
5. 建立 offscreen color/depth 管线。
6. 实现 depth-aware atmosphere/ground compositing。

## 相关文档

- `earth-engine-performance-foundation.md`
- `high-tilt-performance-atmosphere.md`
- `performance-data-stability.md`
- `graphics-pipeline.md`
- `rendering-engine.md`
- `profiling-guide.md`
