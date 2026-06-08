# 地球引擎渲染重点审查清单

本文件用于审查地球引擎渲染相关设计、PR 和 AI 生成代码，重点覆盖深度测试、视锥剔除、地平线剔除、渲染 pass、GPU 资源和可观测性。

它不是通用 3D 渲染清单。审查时必须把每个结论放回本项目的地球尺度坐标、WGS84 椭球、瓦片调度、LOD、移动端 GPU 和可测试接口中判断。

## 审查目标

渲染审查至少要回答：

- 内容为什么应该被画出来？
- 内容为什么可以被剔除？
- 深度关系为什么稳定？
- 当前帧画的是不是来自最新 `FrameState`？
- 视觉结果是否能通过测试、debug overlay 或诊断快照解释？

如果只能说“看起来没问题”，则审查证据不足。

MVP 3D globe 主链路必须先执行 `engine-infrastructure-contracts.md` 中的固定规则。该主链路里的 basemap tile depth test、depth write、backface culling、pass 顺序和 render gate 不是开放式审查项；审查只允许确认实现是否遵守固定规则，不能把关闭 depth/culling 当作普通权衡。

## 必须引用的项目约定

审查渲染、culling、depth、shader、picking 或 GPU 资源时，应至少对照：

- `earth-coordinate-systems.md`
- `rendering-engine.md`
- `graphics-pipeline.md`
- `engine-math-algorithms.md`
- `algorithm-implementation-details.md`
- `shader-interface.md`
- `tiles-terrain-lod.md`
- `basemap-tile-rendering.md`
- `debugging-observability.md`
- `performance-data-stability.md`

涉及 3D Tiles 时，还必须对照 `three-d-tiles.md`。

## 坐标空间审查

- 是否明确顶点从 `cartographic/geographic` 到 `ECEF`、`camera-relative`、`view`、`clip`、`NDC`、`screen` 的完整路径？
- CPU 侧权威世界坐标是否使用 double 或等价精度？
- GPU 侧是否遵守 `shader-interface.md`，用 camera-relative position 输入 shader？
- 是否避免把经纬度、Web Mercator、ECEF、ENU、screen 坐标混在同一算法里？
- `model`、`view`、`projection`、`modelViewProjection` 的列主序和乘法顺序是否与 GLM / shader 约定一致？
- 法线是否使用正确的 normal matrix，而不是直接复用位置矩阵？
- tile、模型、3D Tiles content 的 transform 是否在正确层级应用？

高风险信号：

- shader 中直接修正 provider 偏移或 CRS 问题。
- 用 `float32` 保存权威 ECEF 坐标。
- culling 用世界坐标，bounding volume 却是 camera-relative 坐标，或反过来。
- region 单位是 radian，却按 degree 处理。

## 深度测试审查

- near/far plane 策略是否明确，并适配近地、远地和整球可见场景？
- 是否评估过 near plane 过小、far plane 过大造成的 depth precision 问题？
- 是否需要 logarithmic depth 或 reversed-Z？如果需要，目标后端是否支持，Metal / GL ES / Vulkan 行为是否一致？
- MVP globe / terrain / basemap 是否严格遵守 `engine-infrastructure-contracts.md` 的固定 depth test / depth write 规则？
- 非 MVP 特殊对象（classification、模型、label、atmosphere、云雾、水面、地下对象）是否有独立 depth test / depth write 策略和例外证据？
- 透明对象默认不得 depth write；若特殊对象需要 depth write，是否有独立 pass、排序规则和截图/测试证据？
- 贴地 polygon 是否通过 terrain draping、polygon offset、depth offset 或 classification pass 处理，而不是随意抬高高度？
- depth prepass、color pass、picking pass、shadow pass 的 depth state 是否一致或差异有解释？
- depth buffer 格式、清理时机、framebuffer 尺寸和 MSAA resolve 是否明确？

常见问题：

- 透明面写 depth，导致后续对象消失。
- 大气、云、雾、水面与地形混合顺序依赖魔法 render order。
- 高空看整球时 z-fighting，低空又因为 near plane 太大裁掉近处对象。
- picking pass 和 color pass 的 depth state 不一致，导致看得到但点不到。

## 视锥剔除审查

- frustum plane 提取是否有单元测试？
- frustum plane 所在坐标空间是否与 bounding volume 一致？
- tile、terrain、3D Tiles、模型、标注分别使用什么 bounding volume？
- bounding volume 是否保守覆盖实际几何，特别是地形高程、模型 transform、3D Tiles transform？
- 剔除策略是否保守，避免错误剔除可见内容？
- 相机快速旋转、缩放、tilt 变化时是否不会出现瓦片闪烁或突然消失？
- 是否有 debug overlay 显示 frustum、bounding volume 和剔除状态？

常见问题：

- 用 2D viewport bbox 代替 3D globe 可见性判断。
- tile bounds 未考虑反经线、极区或 provider bounds。
- bounding sphere 太紧，地平线附近瓦片被误剔除。
- culling 结果跨帧复用，但没有绑定 `frameId` 或最新相机状态。

## 地平线剔除审查

- 是否区分 frustum culling 和 horizon culling？
- MVP basemap 背面瓦片是否在 render gate 前被 horizon culling 或前半球门禁剔除？
- horizon culling 是否基于椭球或项目声明的近似球体模型？
- 高空、低空、地平线附近是否分别验证？
- horizon culling 失败时是否优先保守显示，而不是直接隐藏？
- 是否有调试字段显示 tile 被 frustum、horizon、LOD 还是 availability 剔除？

常见问题：

- 只做 frustum culling，导致背面瓦片仍参与请求和渲染。
- horizon 算法过激，地平线附近瓦片闪烁。
- 用球体近似但没有说明误差范围，和 WGS84 椭球坐标混用。

## 背面与遮挡剔除审查

- MVP globe / terrain / basemap 是否保持 backface culling 开启，且 mesh winding 与正面规则一致？
- globe、terrain tile、polygon overlay、model 的正反面规则是否一致？
- 非 MVP 贴地面、地下对象、室内场景若需要关闭或分场景配置 backface culling，是否走了例外流程？
- occlusion culling 是否有明确数据结构和保守性证明？
- 遮挡剔除失败时是否能通过 debug overlay 定位？

高风险信号：

- 为了修复消失问题直接全局关闭 backface culling，但没有分析 winding 或坐标空间。
- 用不可解释的遮挡缓存跨相机帧复用。

## LOD 与 SSE 审查

- 影像瓦片是否基于屏幕像素覆盖、原始分辨率、DPR 和 provider zoom 限制选择 LOD？
- 地形是否基于 geometric error 或 screen-space error？
- 3D Tiles 是否遵守 tileset 的 `geometricError`、`refine`、`boundingVolume` 和 `transform`？
- LOD 是否有 hysteresis，避免临界值来回切换？
- LOD 计算是否输出可调试字段，例如 distance、SSE、selected/refined、fallback？
- 渲染 LOD 数据是否没有被误用于严肃分析？

常见问题：

- 只按相机高度或距离加载瓦片。
- 3D Tiles 忽略 ADD / REPLACE refinement。
- parent fallback 和子瓦片替换没有 all-ready 或 crossfade 策略，导致画面块状跳变。

## 渲染 Pass 审查

- 每个 pass 的输入、输出、framebuffer、清理规则、depth state、blend state 是否明确？
- color、depth、picking、shadow、classification、terrain depth、postprocess、atmosphere、label pass 是否按职责拆分？
- picking pass 是否使用专用 shader 和稳定 feature id 编码？
- picking framebuffer 是否使用真实 framebuffer 像素尺寸，并处理 DPR？
- 后处理是否不会改变业务分级颜色、告警颜色或科学栅格语义？
- pass 之间是否避免不必要的 render target store/load，尤其是在移动端 TBDR GPU 上？

常见问题：

- color pass 材质直接复用于 picking pass，导致光照、透明或后处理污染 id。
- MSAA 后读取 picking id，没有明确 resolve 或禁用策略。
- depth texture 尺寸与 viewport / DPR 不一致。

## 透明与混合审查

- 每类透明对象是否声明 blend mode、premultiplied alpha、depth test、depth write、sort key？
- label、billboard、云、雾、水、大气、半透明 polygon 是否有稳定顺序？
- 是否需要 order-independent transparency？如果不需要，限制条件是什么？
- 透明对象是否避免过度使用 discard / clip，尤其是移动端 TBDR GPU？

高风险信号：

- 只通过 render order 魔法数字解决透明排序。
- 透明业务图层参与 depth write，遮挡后续图层。

## GPU 资源生命周期审查

- shader、pipeline、material、texture、buffer、framebuffer 是否有所有权和释放路径？
- 是否避免每帧创建 GPU 资源？
- 图层隐藏、瓦片卸载、样式切换、场景销毁时资源是否释放？
- Metal resource eviction、GL ES context lost、Vulkan device lost 是否有恢复策略？
- texture cache、buffer cache、raw cache、decoded cache 是否有预算和淘汰策略？
- cache key 是否包含 provider、layer、tile key、style/version、timeKey？

常见问题：

- 只实现资源创建，不实现 dispose。
- 图层不可见但 texture / buffer 仍常驻。
- 网络返回后直接上传过期瓦片纹理。

## 调度与帧状态审查

- 渲染决策是否只基于当前最新 `FrameState`？
- 网络回调是否只更新 tile 状态，而不是直接修改最终 render set？
- request、decode、upload、render 是否分阶段，并有取消或过期检查？
- 快速移动相机后，旧请求结果是否不会覆盖当前场景？
- request queue、texture upload、render queue 是否有优先级和并发限制？
- 静态场景是否避免无意义 continuous render loop？

高风险信号：

- `desiredTiles`、`requestTiles`、`renderTiles` 混为一个集合。
- 线程池任务完成后不检查 frameId / generation / cancellation。

## Debug 与验收审查

渲染相关改动至少应提供一种可复现证据：

- 单元测试：frustum plane、AABB/sphere intersection、tile bounds、screen-to-ray、feature id encode/decode。
- 截图或录屏：近地、远地、整球、高纬、反经线、快速旋转、快速缩放。
- Debug overlay：frustum、bounding volume、tile boundary、LOD error、depth visualization、wireframe。
- 诊断快照：camera、visible tiles、culled tiles、request queue、cache、renderer resource stats。
- 性能指标：draw calls、triangles、texture count、buffer count、GPU memory estimate、frame time。

必须验证：

- 首屏非空白。
- 旋转、缩放、平移正常。
- 近地和远地不明显抖动。
- 地形和影像没有明显错位。
- 瓦片边界没有明显裂缝。
- 快速移动相机不会显示大量过期瓦片。
- 销毁场景或切换图层后资源能释放。

## 审查结论模板

建议在 PR 或实现说明中按以下格式给结论：

```text
坐标空间：
- 输入：
- 内部：
- GPU：
- 输出：

Depth / Pass：
- depth test：
- depth write：
- near/far：
- transparent strategy：
- picking strategy：

Culling / LOD：
- frustum：
- horizon：
- bounding volume：
- LOD/SSE：
- fallback：

资源与调度：
- GPU resources：
- cache key：
- cancellation：
- frame state：

验证：
- tests：
- debug overlay：
- screenshots：
- performance：

剩余风险：
- antimeridian / polar：
- weak network：
- low-end device：
- context lost：
```

## 阻断项

出现以下情况时，不应合并渲染相关改动：

- 坐标空间或单位不明。
- culling 只能通过肉眼验证，没有测试或 debug 输出。
- depth test/write 状态不明，尤其是透明对象和 picking pass。
- 旧请求或旧线程任务可能覆盖当前帧状态。
- 每帧创建 shader、pipeline、texture、buffer 或 framebuffer。
- cache key 缺少 provider / layer / style / time 信息。
- 资源没有释放路径。
- 为了修复视觉问题在 shader 中加入 CRS、provider 偏移或业务纠偏。
- 性能优化以删除 CRS 校验、几何合法性检查或错误处理为代价。
