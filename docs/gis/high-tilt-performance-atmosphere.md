# 高倾斜视角性能与大气合成路线

本文记录高倾斜、近地平线视角下的性能与视觉策略。目标是保持清晰度和跟手性，不通过放宽 tile SSE、降低 LOD 或用雾遮盖问题来换 FPS。

## 当前问题

高倾斜视角会同时放大几个成本：

- 屏幕上 surface tile 覆盖面积大，fragment 和纹理采样压力上升。
- 远处地平线 tile 被压扁，tile 边界、父子替换和 coverage 缝隙更容易被看到。
- 大气、雾、天空和地表如果不在同一合成模型内，会产生浅色边、蓝色硬边或地表洗白。
- tile upload、transition、plan rebuild 如果集中在交互帧，会造成不跟手和掉帧。

这些问题应拆开处理：tile 选择负责清晰度和必要 tile 集合，surface pass 负责地表基础颜色，atmosphere/ground compositing 负责空气透视和天空过渡，upload scheduler 负责帧时间稳定。

## 不采用的策略

不要把以下手段作为主策略：

- 临时放宽 SSE 或降低高倾斜 LOD。
- 把低空 imagery boost 实现为“拆出更多 surface tile draw call”。
- 在 surface tile fragment shader 中堆复杂雾、大气散射或遮盖逻辑。
- 依赖 tile transition alpha 把地球淡到天空背景。
- 用 debug overlay、背景色或全屏雾掩盖 tile 缝、父子同画、upload 抖动。

这些方法会降低清晰度、制造 tile 块状雾、增加 fragment 成本，或让视觉问题变成性能问题。

## Provider / Scheme 对齐

高倾斜 tile 消失或 `unsupported` 大量增长时，先检查 provider 的 tiling scheme 是否和 TilePlan scheme 一致。标准 XYZ/WebMercator provider（例如当前高德卫星和路网模板）不应挂到 `OpenGlobus-Earth` 三分区 scheme 上，否则高倾斜会遍历 north/south polar group，图层侧再丢弃 provider 不支持的 tile，造成无效候选、draw/plan 压力和错误诊断。

如果 provider 不支持极区或自定义分组，应在 scheme 层或 TilePlan 阶段避免生成这些候选，而不是依赖图层请求阶段过滤。

## SurfaceTile 职责边界

SurfaceTile pass 应尽量成为高频主路径的最小成本渲染：

- imagery texture sample。
- 可选 water mask。
- layer opacity。
- 必要的 terrain/material light。
- depth write，用于后续 depth-aware compositing。

SurfaceTile pass 不应负责：

- 远距雾化。
- 地平线空气透射。
- 天空和地表交界过渡。
- 大气散射。
- LOD 或 tile handoff 遮丑。

如果需要保留短期雾，只允许极轻、可关闭、不会影响近中距离清晰度的过渡项；长期应移出 surface tile shader。

## Shader Variant 计划

高倾斜主路径应避免每个 fragment 都执行无用分支。至少拆分以下 variant：

- `SurfaceTileOpaqueNoWater`：一次 imagery sample，无 water branch，无 blend。
- `SurfaceTileOpaqueWater`：imagery sample + water mask sample。
- `SurfaceTileLayerBlendNoWater`：透明图层或 layer opacity 小于 1 时使用。
- `SurfaceTileLayerBlendWater`：透明图层 + water mask。

主路径应优先保证 `SurfaceTileOpaqueNoWater` 占大多数 draw。无 water mask 的 tile 不应携带 water mask sampler、branch 或第二纹理绑定。

## Depth-Aware Atmosphere/Ground Compositing

长期正确方向是独立的 depth-aware atmosphere/ground compositing，而不是 surface tile 内雾化。

目标管线：

```text
Sky/background pass
Surface opaque pass -> color + depth
Atmosphere/ground composite pass -> read color/depth -> final color
Vector/debug overlay
UI
```

职责：

- 对 sky pixel 做天空大气散射。
- 对 ground pixel 根据 depth/ray distance/相机高度/太阳方向做空气透视。
- 用同一套颜色和光学深度模型处理地平线边界。
- 避免 surface tile shader 为远距雾付 per-fragment 成本。

Android GLES 初期可先做低风险版本：

- offscreen color texture。
- depth texture 或可采样 depth attachment。
- surface pass 写 color/depth。
- fullscreen composite pass 采样 depth/color。
- 输出到默认 framebuffer。

如果 depth texture 支持或设备兼容性不足，允许先做 screen-space horizon transition pass 作为临时实验，但不得视为最终方案。

## Screen-Space Horizon Transition 的定位

screen-space horizon transition pass 只适合作为短期过渡：

- 快速减少高倾斜地平线浅色边或硬蓝边。
- 不改 tile selection，不降低 LOD。
- 成本可控，容易 A/B。

限制：

- 不知道真实地表深度或地形遮挡。
- 高度、FOV、地形起伏、极端斜视下可能露出新假边。
- 不应承担真实雾、大气或地表透射职责。

因此它可以用于验证观感，但最终应被 depth-aware compositing 替代。

## Draw/State 优化

Surface tile 绘制顺序：

```text
OpaqueNoWater tiles, front-to-back
OpaqueWater tiles, front-to-back
LayerBlendNoWater tiles, back-to-front if needed
LayerBlendWater tiles, back-to-front if needed
Atmosphere/ground composite
Overlay
```

要求：

- opaque surface tile front-to-back，保留 early-z 机会。
- 按 shader variant 分桶，减少 program 切换。
- 按纹理和 layer 分组，减少 texture bind。
- 透明或 layer opacity 小于 1 的 tile 与 opaque 主路径分离。
- debug overlay 和 vector overlay 不参与默认性能基准。
- 避免父子 tile 同屏重复画，除非 transition 设计明确且有预算。

## Upload 与 Transition 预算

tile upload 和 renderable handoff 必须有稳定帧预算：

- 每帧限制 texture upload 数量或上传耗时。
- upload 完成后不要在同一帧重复 rebuild 全量 plan。
- request completion、GPU upload、renderable handoff 分阶段。
- 过期请求回调必须清理 in-flight/requested 状态。
- parent fallback 到 child exact 的切换不应引入整 tile alpha 混合。
- interaction frame 应优先保证输入响应和当前可绘制集合稳定。

高倾斜掉帧如果伴随 upload spike，应先处理 upload/rebuild，而不是放宽 SSE。

## 固定 QA 视角

必须保留固定视角用于 A/B：

- near-ground high tilt。
- mid-altitude oblique。
- horizon/limb。
- normal nadir。
- fast interaction。

每个视角至少记录：

- FPS。
- draw calls。
- visible tiles / renderSurface tiles。
- shader variant 分布。
- texture upload count/time。
- tile plan/update time。
- render submit time。
- glError / shader compile link error。
- screenshot。

当前 Android QA 可以使用固定高倾斜预设按键切视角，避免手势复现不稳定。

## 验收标准

性能：

- 固定高倾斜视角稳定接近 60 FPS。
- 无 `glError`。
- draw call、renderSurface tile 数可解释，且 A/B 可复现。
- 交互期间没有 upload/rebuild 大尖峰。

清晰度：

- 不通过 SSE 放宽降低高倾斜目标清晰度。
- 近中距离地表不被雾洗白。
- 远处地平线过渡自然，不出现明显浅色 tile 带或硬蓝边。

架构：

- surface tile shader 主路径保持简单。
- water/no-water、opaque/blend variant 分离。
- 大气/雾/地平线过渡逐步迁移到 depth-aware compositing。
- visual-only 效果不得掩盖 tile selection、LOD、upload 或 transition 缺陷。

## 推荐实施顺序

1. 建立固定高倾斜 QA 视角与日志采集。
2. 移除 surface tile shader 中的远距雾主逻辑，保留最小可控 fallback。
3. 拆分 `SurfaceTileOpaqueNoWater` 和 `SurfaceTileOpaqueWater`。
4. 按 variant 分桶并保持 opaque front-to-back。
5. 增加 upload budget 和 handoff 指标。
6. 实现 depth-aware atmosphere/ground compositing 的 offscreen 管线。
7. 用 composite pass 统一天空、地表和地平线空气透视。
8. 删除临时 screen-space/haze workaround。

## 参考对齐

- cesium-native：tile selection、LOD、horizon culling、raster overlay 语义优先对齐。
- OpenGlobus：atmosphere/deferred compositing、presentation 和 globe 渲染组织可作为表现层参考。
- 本项目：手势、Android MinimalGlobe QA 视角和移动端预算按本项目交互契约执行。
