# 地球引擎基础设施契约

本文件用于把地球引擎的基础设施先落地，避免后续实现只按局部记忆、视觉直觉或临时修补推进。

任何涉及坐标、瓦片、相机、渲染、底图、地形、拾取、调度、缓存、shader、GPU 资源或移动端平台桥接的实现，都必须先对照本文件。通过本文件不能替代专题文档；它负责说明基础设施之间如何咬合，专题文档负责算法和接口细节。

## 总原则

- 基础设施不是“有类名”就完成，必须有契约、边界、测试、debug 证据和失败策略。
- 任何视觉结果都必须能被坐标、瓦片、相机、渲染状态和诊断数据解释。
- 不得用 shader 偏移、关闭 depth/culling、抬高高度、魔法 render order 等视觉 hack 掩盖坐标或数据错误。
- 每个模块只能做自己的职责；跨层修补必须先记录为风险，并补上验证。
- 默认场景也必须验收。首屏相机、底图 CRS、provider 覆盖范围和渲染状态必须匹配。

## MVP 3D Globe 固定规则

本节是 MVP 3D globe 底座的硬规则。后续 AI 不得把这些规则表述为“建议”“可选”或“看情况”。除非任务明确是在实现非 MVP 的特殊 pass，并且先补充设计文档、测试和 debug 证据，否则不得改动这些默认行为。

### Pass 顺序

MVP 默认 pass 顺序固定为：

```text
1. clear color + depth
2. SurfaceTile depth and color (ellipsoid or terrain surface + imagery attachments)
3. vector overlays
4. debug overlay
```

固定规则：

- `SurfaceTile` 必须是标准地球表面几何和 depth 的唯一来源。
- 标准底图影像必须作为 `SurfaceTile` 的 imagery attachment 渲染，不得作为另一套独立共面 tile mesh 参与 MVP 主链路。
- `globe` 可作为无瓦片 fallback 或 loading background，但不得与标准底图长期形成 `GlobeCommand + BasemapTileCommand` 共面竞争。
- debug overlay 可以有独立可视化策略，但必须可关闭，且不得作为正常渲染正确性的依据。
- picking pass 必须独立设计，不得直接复用 color pass 的视觉材质作为 feature id 输出。

正式主链路见 `surface-tile-mainline.md`。该文档优先于旧的独立 basemap tile pass 方案。

### SurfaceTile Render Gate

标准 XYZ / TMS / WMTS 底图进入最终 `renderSurfaceTiles` 前，必须通过以下门禁：

```text
current FrameState
-> provider bounds / availability
-> TileScheme bounds
-> frustum visibility
-> horizon or front-hemisphere visibility
-> LOD selection
-> cache / fallback selection
-> SurfaceTile geometry ready
-> ImageryAttachment ready or parent fallback ready
-> renderSurfaceTiles
```

固定规则：

- `visibleTiles`、`desiredSurfaceTiles`、`requestImageryTiles`、`fallbackAttachments`、`renderSurfaceTiles` 必须区分。
- 只有 `renderSurfaceTiles` 可以生成标准底图 `SurfaceTileCommand`。
- 背面 surface tile 不得进入 `renderSurfaceTiles`。如果 horizon culling 暂未完成，必须有等价的前半球可见性门禁；不能通过关闭 depth/culling 让背面瓦片交给 GPU “碰运气”。
- provider 覆盖范围外的 imagery tile 不得进入 `requestImageryTiles` 或 `ImageryAttachment`，除非明确使用 parent fallback 或透明缺失策略。
- 网络回调只能更新 surface / imagery tile 状态，不能直接修改 `renderSurfaceTiles` 或提交 draw command。
- `renderSurfaceTiles` 必须绑定当前 `frameId` 或 generation；过期 generation 的 surface 或 imagery attachment 不得污染当前帧。

### SurfaceTile GPU State

MVP 3D globe 的标准地表 GPU 状态固定为：

```text
depthTest  = true
depthWrite = true
cullFace   = true
blend      = false, when only one opaque base imagery attachment is used
blend      = true, when multiple imagery attachments / opacity / alpha require it
```

固定规则：

- 不得通过 `depthTest=false` 修复白屏、闪烁、共面或瓦片消失。
- 不得通过 `cullFace=false` 修复背面瓦片、winding、地平线或可见性问题。
- 标准底图不得再通过独立共面 imagery mesh 与 globe/terrain 竞争 depth；imagery 必须作为 `SurfaceTile` material/attachment。
- 如果 surface mesh winding 与 backface culling 不匹配，必须修 winding 或 shader 顶点顺序，并补测试；不得关闭 culling。
- 如果某个特殊图层确实需要双面渲染，它不得复用 MVP `SurfaceTileCommand`，必须有独立 layer type、独立 pass 说明和截图/测试证据。

### Globe / Terrain GPU State

MVP 3D globe 的 globe fallback 或 terrain 主表面状态固定为：

```text
depthTest  = true
depthWrite = true
cullFace   = true
blend      = false
```

固定规则：

- `SurfaceTile` / terrain 是地球表面遮挡的基准，不能关闭 depth write。
- globe mesh、terrain mesh 和 surface tile mesh 的正面朝向必须一致。
- terrain 替代 ellipsoid surface 时，必须提供等价 depth 行为；不能让底图绕过 terrain 遮挡。

### 默认相机与底图 CRS

固定规则：

- 默认首屏相机必须避开 Web Mercator 极区裁剪主导画面的问题。
- 如果默认 provider 是 Web Mercator，首屏中心应落在 Web Mercator 有效纬度范围内。
- 从极区、高纬、反经线或 provider 覆盖边界启动时，必须把它作为专门测试场景，而不是默认验收场景。
- 默认首屏必须有截图或像素验收：地球完整、无球包球、无背面瓦片无遮挡绘制。

### 例外流程

任何改动如果要违反本节固定规则，必须同时满足：

1. 在相关专题文档中记录为什么 MVP 规则不适用。
2. 增加独立 layer/pass/type，而不是复用 MVP `SurfaceTileCommand`。
3. 提供单元测试或集成测试证明状态切换不会污染 MVP 主链路。
4. 提供 debug overlay 或诊断快照字段，能解释该例外为什么被画出来。
5. 在最终说明中列出风险、边界场景和回滚方式。

不满足以上条件时，违反固定规则的实现应直接拒绝或回退。

## 基础设施链路

地球引擎的最小可靠链路如下：

```text
Geodesy / CRS
-> TileScheme / CrsProfile
-> Camera / FrameState
-> TilePlan / LOD / Culling
-> Provider / Scheduler
-> Raw / Decoded / Texture Cache
-> RenderCommand
-> RenderDevice / Shader / GPU State
-> Picking / Interaction
-> Diagnostics / Tests / Visual Acceptance
```

后续实现不得跳过中间环节。例如底图瓦片不能从 provider 回调直接进入最终 draw list；网络返回只能更新 tile 状态，当前帧画什么必须由最新 `FrameState` 和 render queue 决定。

## 1. Geodesy / 坐标内核

职责：

- 定义 WGS84 ellipsoid、Cartographic、Cartesian3/ECEF、ENU、Ray、Rectangle。
- 集中实现经纬高到 ECEF、ECEF 到经纬高、局部 ENU、ray 与椭球求交。
- 明确 CRS、坐标顺序、单位、高度基准和误差容差。

禁止：

- 在 UI、shader、provider URL 拼接或业务层散落坐标转换。
- 用球体公式冒充 WGS84 椭球算法，除非明确标注近似和误差。
- 内部混用 degree/radian。
- 用 float32 保存权威 ECEF 世界坐标。

做对的证据：

- `cartographic <-> ECEF`、ENU、ray/ellipsoid intersection 有单元测试。
- 测试覆盖赤道、本初子午线、极区、高度非 0、负高度、degree/radian 误用。
- Picking 输出说明 CRS、高度来源和单位。

相关文档：

- `earth-coordinate-systems.md`
- `core-api-contracts.md`
- `algorithm-implementation-details.md`
- `spatial-calculation.md`

## 2. CRS / 投影 / Provider 坐标

职责：

- 区分数据 CRS、服务商 CRS、瓦片矩阵 CRS 和引擎统一世界坐标。
- 定义 `CrsProfile`，处理 EPSG:4326、EPSG:3857、GCJ-02、BD-09、百度墨卡托、私有投影或局部坐标。
- 让 provider 的坐标约定在接入前可测试、可记录、可复核。

禁止：

- 把高德、腾讯、百度或私有瓦片当作标准 Web Mercator。
- 在 shader 中修正 provider 偏移。
- 把视觉对齐当作无偏移证明。
- 科学栅格或分析数据未经说明就做视觉重采样后参与计算。

做对的证据：

- 每个 provider 在 `provider-compatibility-matrix.md` 记录 CRS、tile scheme、偏移模型、覆盖范围和兼容等级。
- 多瓦片体系通过控制点验收，证明无系统性偏移。
- CRS 转换使用成熟 GIS 库或项目认可的等价实现；若为自研近似，必须说明误差和适用范围。

相关文档：

- `multi-tile-schemes.md`
- `provider-compatibility-matrix.md`
- `data-provider-contracts.md`
- `trusted-sources.md`

## 3. TileScheme / 瓦片矩阵

职责：

- 定义 `z/x/y`、TileMatrixSet、y 轴方向、bounds、min/max zoom、tile size、provider bounds。
- 提供 `tileToRectangle`、`positionToTile`、`getTileRange` 等可测试接口。
- 处理 Web Mercator 纬度裁剪、世界边界、反经线、极区和私有覆盖范围。

禁止：

- 只凭 URL 模板假设瓦片方案。
- 裸用 `z/x/y` 作为全局缓存 key。
- 在 3D globe 上直接用 2D viewport bbox 选择瓦片。
- 忽略 y 轴方向导致 XYZ/TMS 颠倒。

做对的证据：

- tile id 到 bounds、bounds 到 tile range、y 轴方向、zoom 边界、反经线或世界边界有测试。
- Debug overlay 能显示 tile boundary、z/x/y、scheme、provider bounds 和 tile 状态。
- 非标准 tile scheme 有控制点或 fixture 验收。

相关文档：

- `tiles-terrain-lod.md`
- `basemap-tile-rendering.md`
- `test-fixtures.md`

## 4. Camera / FrameState / 视锥

职责：

- 定义相机 position、direction、up/right、view/projection、near/far、FOV、pick ray。
- `FrameState` 是每帧计算的唯一上下文来源，包含相机、viewport、DPR、time 和 diagnostics。
- 支持 frustum culling、horizon culling、相机碰撞、拖拽和缩放。

禁止：

- Camera 直接请求瓦片或修改图层数据。
- 用不匹配的默认相机启动场景，例如从 Web Mercator 不覆盖的极区正上方看底图，却把异常视觉当作渲染问题。
- 忽略 iOS points/pixels、Android dp/px、DPR、安全区或 surface offset。
- near/far 随意设置，导致整球或近地深度精度失控。

做对的证据：

- `screen -> pick ray`、frustum plane、相机 basis、viewport aspect 有测试。
- 默认启动场景有截图验收：首屏非空白、地球完整、相机不穿地、不明显抖动。
- Debug overlay 或诊断快照能输出 camera position、height、heading/pitch/roll、near/far、frustum。

相关文档：

- `interaction-system.md`
- `algorithm-implementation-details.md`
- `rendering-review-checklist.md`

## 5. TilePlan / LOD / Culling

职责：

- 基于最新 `FrameState` 计算 visible tiles、desired tiles、request tiles、render tiles 和 fallback tiles。
- 区分 frustum culling、horizon culling、provider availability、LOD、cache state。
- 使用明确 LOD 指标，例如屏幕像素覆盖、分辨率、DPR、SSE、geometric error。

禁止：

- 把 `desiredTiles`、`requestTiles`、`renderTiles` 混成一个集合。
- 只按“相机近就高 zoom”的口头规则选择 LOD。
- horizon culling 缺失时仍让背面瓦片进入最终渲染。
- 相机变化后复用旧 tile plan 而不绑定 `frameId` 或 generation。

做对的证据：

- TilePlan 输出能说明每个 tile 被选中、剔除、请求或 fallback 的原因。
- 快速旋转、连续缩放、高纬度、反经线、弱网下不会大面积白屏或显示过期瓦片。
- LOD 有 hysteresis，debug overlay 能显示 LOD error / SSE。

相关文档：

- `basemap-tile-rendering.md`
- `tiles-terrain-lod.md`
- `performance-data-stability.md`

## 6. Provider / Scheduler / 异步边界

职责：

- Provider 负责 metadata、availability、request、parse、attribution 和 dispose。
- Scheduler 负责请求优先级、并发、取消、重试、过期检查和弱网策略。
- 网络、解析、解码、上传和渲染必须分阶段。

禁止：

- 网络回调直接修改最终可见集或直接提交渲染命令。
- 旧相机状态下的请求返回后覆盖当前帧。
- 无限重试失败瓦片。
- 失败类型不区分 404、401/403、timeout、decode error、network unavailable、provider out of bounds。

做对的证据：

- 请求成功、失败、取消、超时、重试有集成测试。
- 快速移动相机后，过期请求不会污染当前画面。
- Debug overlay 或诊断快照能显示 queued/loading/ready/failed/evicted、request priority 和 cancellation/generation。

相关文档：

- `data-provider-contracts.md`
- `error-recovery.md`
- `threading-architecture.md`
- `security.md`

## 7. Cache / Texture Upload / 资源预算

职责：

- 分离 RawCache、DecodedCache、TextureCache、RenderTileCache。
- GPU texture 和 buffer 通过 `RenderDevice` 创建，主线程上传，后台线程只产出 CPU 数据。
- 缓存有预算、淘汰策略和资源释放路径。

禁止：

- cache key 只包含 `z/x/y`。
- 一帧无限上传所有 ready 图片。
- 图层隐藏、瓦片卸载、样式切换、场景销毁时资源不释放。
- context lost / device lost 后继续使用旧 GPU handle。

做对的证据：

- cache key 包含 provider、layer、tile key、style/version、time、DPR 或 scale 等影响资源的字段。
- 每帧纹理上传数量受控，移动端建议先以不超过 4 张/帧为默认预算。
- Diagnostics 能显示 texture count、buffer count、GPU memory estimate、每帧资源创建次数。

相关文档：

- `basemap-tile-rendering.md`
- `graphics-pipeline.md`
- `performance-data-stability.md`
- `error-recovery.md`

## 8. RenderCommand / RenderDevice / GPU 状态

职责：

- Layer 只把数据转换成 `RenderCommand`；Renderer/RenderDevice 只负责执行 GPU 命令。
- `RenderCommand` 必须明确 pass、shader、vertex/index buffer、uniforms、textures、depth、blend、cull、bounding volume。
- RenderDevice 抽象 Metal、OpenGL ES、Vulkan 的能力差异和资源生命周期。

禁止：

- RenderCommand 中包含网络请求、业务权限判断或 CRS 临时修正。
- 为了修复贴地共面问题全局关闭 depth test 或 cull face。
- 用魔法 render order 长期维护透明、深度或后处理。
- 每帧创建 shader、material、texture 或 buffer。

做对的证据：

- 每类对象声明 depth test、depth write、cull face、blend mode 和 pass。
- 整球、近地、远地、透明对象、picking pass 分别有截图或测试验收。
- 图层切换、瓦片卸载、场景销毁后 GPU 资源统计下降。

相关文档：

- `graphics-pipeline.md`
- `rendering-engine.md`
- `rendering-review-checklist.md`
- `shader-interface.md`

## 9. Shader Interface / 坐标空间

职责：

- 统一 vertex attribute layout、uniform block、camera-relative 坐标、precision、texture sampler 和 pass 类型。
- 保证 MSL、GLSL ES、SPIR-V 变体在同一输入下语义一致。
- 让 shader 只表达已校正的数据渲染，不处理业务或 CRS 问题。

禁止：

- shader 中直接处理 provider 偏移、权限、业务状态或临时纠偏。
- attribute layout 与 C++ vertex struct 不一致。
- color pass shader 直接复用于 picking pass，导致光照、透明、后处理污染 id。
- 对移动端 GPU 使用桌面级 shader 复杂度和纹理规模。

做对的证据：

- shader 编译通过，attribute/uniform 与 C++ 侧契约一致。
- MSL 与 GLSL ES 截图或像素对比差异在可解释范围内。
- Picking pass 使用专用 shader 或明确规则，DPR 和 framebuffer 尺寸正确。

相关文档：

- `shader-interface.md`
- `shader-compilation.md`
- `graphics-pipeline.md`

## 10. Picking / Interaction / 编辑

职责：

- 将原始平台输入归一化为内部 `InputEvent`。
- `screen -> ray -> ellipsoid/terrain/feature/model` 的输出必须说明 CRS、高度来源、单位和 hit type。
- 绘制、编辑、测量和选择必须有状态机、撤销/重做和取消清理。

禁止：

- 工具直接读取平台原始坐标并绕过 DPR / viewport / safe area 处理。
- 看得到但点不到，或点到结果不说明高度基准。
- 编辑工具没有取消、撤销或临时 feature 清理。
- 使用平面测量冒充测地线测量。

做对的证据：

- 不同 DPR、屏幕尺寸、安全区、Android density 下 picking 准确。
- center pick、sky pick、地形 pick、feature pick 有测试或可复现场景。
- 测量结果显示单位和计算模型。

相关文档：

- `interaction-system.md`
- `spatial-calculation.md`
- `algorithm-implementation-details.md`

## 11. Terrain / 3D Tiles / 高级数据

职责：

- 地形明确高程基准、单位、网格格式、LOD、skirt、法线、接缝和地形拾取。
- 3D Tiles 明确 tileset traversal、bounding volume、transform、geometricError、SSE、content 生命周期。
- 新增数据类型前先在 `engine-data-catalog.md` 登记分类、坐标、单位、时间、LOD、样式、权限、缓存和验收。

禁止：

- 在底图链路未稳定前接入复杂地形或 3D Tiles。
- 地形和影像 LOD 混成一套而不说明对齐策略。
- tileset unload 后不释放 GPU 资源。
- 把视觉-only 环境效果当作分析数据。

做对的证据：

- 地形与影像对齐，接缝不明显，地形拾取返回高度。
- 3D Tiles 渐进加载，picking 返回 feature/object id，卸载释放资源。
- 有高纬、反经线、快速缩放、弱网、场景销毁重建回归场景。

相关文档：

- `tiles-terrain-lod.md`
- `three-d-tiles.md`
- `engine-data-catalog.md`
- `environment-atmosphere-weather.md`

## 12. Diagnostics / Debug / 验收闭环

职责：

- Debug overlay、日志分类、诊断快照、单元测试、集成测试、截图验收共同构成正确性证据。
- 每个实现必须说明“如何知道做对了”，而不是只说“看起来正常”。

最低诊断字段：

- camera position / height / heading / pitch / roll
- viewport width / height / DPR
- current CRS / tile scheme / provider
- visible tiles / culled tiles / requested tiles / rendered tiles
- tile state：missing、queued、loading、decoded、texture-ready、rendered、failed、evicted
- request queue、loading count、retry count、generation/frameId
- FPS、frame time、draw calls、texture count、buffer count、GPU memory estimate
- depth/cull/blend state，至少在 debug snapshot 中可见

禁止：

- 只靠肉眼验收复杂 GIS 或渲染逻辑。
- debug overlay 污染产品 UI 或影响正常交互。
- 只输出 console.log 而没有结构化诊断快照。

做对的证据：

- MVP 至少有核心数学测试、tile scheme 测试、picking 测试、双平台截图或像素检查。
- 每次大改后检查快速旋转、连续缩放、高纬、反经线、弱网、图层切换、场景销毁重建。
- 最终说明列出坐标/CRS/单位假设、验证方式、未覆盖风险和下一步测试。

相关文档：

- `debugging-observability.md`
- `engine-testing-acceptance.md`
- `mvp-acceptance.md`
- `verification-checklist.md`
- `engine-development-checklist.md`

## 当前已知高风险组合

以下组合在本项目中必须优先避免或显式验证：

- 默认相机从极区正上方看 Web Mercator 底图，却没有说明 Web Mercator 极区裁剪。
- 底图瓦片贴地后关闭 depth test 和 cull face，导致背面瓦片或过期瓦片无遮挡绘制。
- 用 globe depth 解决贴地瓦片共面，但没有 polygon offset、terrain draping、depth prepass 或可解释替代策略。
- 用 2D bbox 选择 3D globe 可见瓦片。
- provider 回调直接影响当前帧 render set。
- cache key 缺少 provider/layer/style/time/DPR，导致不同图层资源串用。
- shader 里写 CRS 偏移或临时缩放。
- 截图只验证“非空白”，不验证地球完整、瓦片位置、背面遮挡和 debug tile 状态。

## AI 实现前强制问题

后续 AI 在实现或修改地球引擎基础设施前，必须回答：

1. 本次改动属于链路中的哪一段？
2. 输入 CRS、坐标顺序、单位、高度基准是什么？
3. 内部坐标空间和 GPU 坐标空间分别是什么？
4. 当前帧数据是否来自最新 `FrameState`？
5. 是否涉及异步请求、取消、generation 或过期检查？
6. RenderCommand 的 depth/cull/blend/pass 状态是什么，为什么？
7. 如何通过单元测试、debug overlay、诊断快照或截图证明做对？
8. 哪些边界场景没有覆盖，是否已在最终说明中列出？

如果无法回答这些问题，不应继续实现复杂功能；应先补契约、测试或调试观测。
