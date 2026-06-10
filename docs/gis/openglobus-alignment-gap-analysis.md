# OpenGlobus 算法对齐差异记录

本文件记录 `earth-md` 当前地球引擎算法与 `/Users/ldy/Desktop/work/openglobus` 的对齐状态。本文只作为本项目工程判断依据，不表示直接复制 OpenGlobus 实现。

## 对齐口径

- 坐标基准：默认 WGS84 ellipsoid，ECEF 单位为 meter，经纬度内部使用 radian。
- 瓦片体系：标准 XYZ/TMS Web Mercator 只覆盖主 Mercator 区；OpenGlobus Earth 策略还包含北极和南极 LonLat 补丁。
- LOD：不能只用相机高度估算 zoom，应至少用 tile bounding volume 的屏幕投影尺寸、frustum 可见性、地平线/前半球可见性和相机所在 tile 保护。
- 地形：地表主链路应走 `SurfaceTile`，地形高度应在 ECEF surface mesh 构建阶段采样，而不是长期保留独立单位球地形 mesh 主链路。
- 拾取、测地线、相机交互、渲染精度必须明确 miss/invalid 语义、单位和边界情况。

## 当前未严格对齐项

1. 椭球算法：`cartesianToCartographic` 和 `scaleToGeodeticSurface` 需要对齐 OpenGlobus `projToSurface` 的椭球表面投影模型；射线命中应提供明确 miss 语义；测地线 direct/inverse 能力缺失。
2. 三分区瓦片：当前 XYZ/TMS Web Mercator 会 clamp 到约 `±85.05112878°`，缺少 OpenGlobus 的 north/south polar LonLat tile group。
3. LOD/可见性：已有 quadtree 和 projected-size 细分，但缺少可导出的节点状态统计、tile group 统计、相机所在 tile 保护证据和更清晰的可见性原因。
4. 地形主链路：`SurfaceTile` 是标准地表 mesh 与 depth 主链路；地形高度必须进入 ECEF `SurfaceTile` mesh，不得使用独立单位球地形 mesh 替代。
5. 深度与 GPU 精度：相机 near/far 仍是固定值，尚未实现 reversed-Z 或 logarithmic depth。
6. Picking：椭球 miss 当前可能通过零向量表达；复杂 polygon、holes、反经线和 GPU picking pass 仍未完整。
7. 环境光照：太阳方向是简化 Meeus 近似，尚未对齐 OpenGlobus astro 模块级能力。

## 本轮实施范围

本轮先实现用户指定的 1、3、4、5 中与核心算法直接相关的部分：

- 1：补齐椭球 `projectToSurface`、精确 `scaleToGeodeticSurface`、`rayIntersection` 和 Vincenty inverse/direct。
- 3：新增 OpenGlobus Earth 三分区 tile scheme，覆盖 Mercator 主区与 north/south polar tile group。
- 4：增强 `TilePlan` 诊断字段，记录 rendered/walkthrough/not-rendering 节点统计和 tile group 统计。
- 5：新增 `TileSurface::buildTerrainMesh`，在 ECEF `SurfaceTile` mesh 上采样 terrain height，并修正 ellipsoid normal 为椭球法线。

## 已推进的主链路接入

- `TerrainLayer` 已从默认渲染 pass 中退回为地形数据源：负责加载、缓存、采样和提供 `terrainGeneration`。
- `BasemapLayer` 构建 `SurfaceTile` GPU mesh 时会优先查询 `TerrainLayer::findBestTile`，若有覆盖 tile，则使用 `TileSurface::buildTerrainMesh` 生成 ECEF terrain SurfaceTile mesh；否则使用 ellipsoid SurfaceTile mesh。
- `Scene` 不再在 terrain enabled 时先发出独立地形 surface 命令；SurfaceTile 继续作为标准地表 depth 主链路。
- 旧 `TerrainMeshBuilder` 和 `TerrainSurface` render kind 已移除，避免单位球地形路径绕过 SurfaceTile 契约。
- `Diagnostics` 已补充 `terrainCachedTiles`、`terrainGeneration`、`terrainSurfaceMeshes`、`terrainParentFallbackMeshes` 与 `ellipsoidSurfaceMeshes`，用于核对地形数据是否进入 SurfaceTile mesh 构建条件。
- `TileSurface` 已区分采样模型：Mercator 主区继续使用 WebMercator-v；OpenGlobus north/south polar LonLat group 使用 geographic-v，避免极区 SurfaceTile mesh 被 Web Mercator clamp 到 `±85.05112878°`。
- parent terrain fallback 已通过目标 tile 的 WGS84 经纬度在父 `TerrainTile` bounds 内采样，等价于从父 heightmap 矩阵裁剪子 tile 覆盖范围；该行为有单元测试固定。
- SurfaceTile terrain 已默认生成 skirt 作为不同 LOD/未齐备邻接瓦片时的接缝降级策略；当前还不是完整邻接边高程 reconciliation。
- `SurfaceVertex` 已保存 CPU double ECEF authoritative position，同时拆分 `positionHighEcef` / `positionLowEcef`，为后续 shader high/low split 或更严格 camera-relative 上传提供契约。
- `LayerTilePlan` 已区分 `Ready`、`ParentFallback`、`Missing`，parent fallback tile 带 `transitionOpacity`；Diagnostics 已暴露 imagery/terrain transition 和 readiness 计数。
- `LayerTilePlan` 的 imagery 请求已按 OpenGlobus material readiness 语义改为父链先行：高 zoom target 缺纹理时，`requestTiles` 选择从 root 到 target 的第一个缺失且 provider 支持的祖先 tile，并跨 visible leaves 去重；只有父级 fallback 链稳定后才逐步追 exact texture，避免把 `visibleTiles` 直接当作高层级请求风暴。实际 provider 请求启动受 OpenGlobus `loadingBatchSize=12` 对应的 in-flight 预算约束，避免 layer 在高 zoom 期间无限制创建网络/解码线程。
- `ImageryProvider` 已增加 `supportsTile` 与 `providerKeyForTile` 门禁；`XYZImageryProvider` 必须显式启用 OpenGlobus grouped-y 才会请求三分区 tile，并支持 `{tileGroup}` / `{groupedY}` URL 模板占位。
- SurfaceTile normal map 已按 OpenGlobus 数据语义接入：从 surface mesh 的 ECEF/world normal 派生 RGBA8 normal texture，作为 SurfaceTile command 的第二纹理绑定，shader 解码后参与光照；无 normal map 时回退顶点法线。对齐 OpenGlobus 后，normal map 是 terrain/segment readiness 或显式 debug 能力，不是每个 ellipsoid 底图 tile 的默认同步资源；ellipsoid SurfaceTile 默认使用顶点法线，避免高 zoom 下把普通影像瓦片误升级成 terrain normal-map 工作负载。

## Cesium Native 选择器借鉴决策

当前低空局部清晰块的直接成因，是四叉树选出的可渲染节点层级不均匀：中心相机分支继续细分，周边仍停在较粗层级。同一 z18 影像贴到不同大小的 `SurfaceTile` / target 覆盖范围后，屏幕采样密度不一致，视觉上形成局部清晰块。

这不是 OpenGlobus 原版策略错误，而是当前实现还缺少完整配套机制。后续路线采用混合方案：

- 保留 OpenGlobus 风格的 globe segment 表达：三分区 tile scheme、极区处理、地平线可见性、相机所在 segment 保护、terrain/segment 状态语义。
- 借鉴 cesium-native 的选择器状态机：SSE/geometric error、renderable 判断、上一帧 selection state、Ancestor Meets SSE、Kicking、loadingDescendantLimit 和 raster overlay target pixels。
- `equal-zoom` 只能作为诊断或实验开关，用来验证层级不均匀问题；不应成为低空正式策略。正式修复应靠选择器状态、父子替换和 imagery/surface 尺度匹配完成。

对本项目而言，cesium-native 的强项不是某个公式，而是把“是否细分”“是否可渲染”“加载哪个祖先/子孙”“上一帧画过什么”“本帧 render list 是否要回退父级”合成一个闭环。该闭环比当前简化 quadtree 更适合解决低空局部清晰块、缩放闪烁和父子瓦片替换不连续。

计划拆分：

1. 在 `TileQuadTree` 中用更接近 cesium-native 的 SSE 作为主要 refine 条件，避免 camera-inside 分支单独把中心分支拉得过深。
2. 为 `TileNode` 增加 selection state，记录 `Rendered`、`Refined`、`Kicked`、`NotVisited` 以及上一帧原始状态。
3. 为 `SurfaceTile` / imagery / terrain fallback 定义 `isRenderable`，让 render list 决策依赖资源状态，而不是只依赖目标 tile key。
4. 实现简化版 Kicking：子孙未 ready 且上一帧未渲染过时，继续渲染父 tile，同时保留子孙加载。
5. 实现简化版 Ancestor Meets SSE：父 tile 满足 SSE 但不可渲染时，继续允许上一帧深层子孙渲染。
6. 引入 loading descendant limit，避免低空视角一次性追过多深层瓦片。
7. 让 imagery attachment 参考 surface tile 屏幕覆盖和目标像素，避免固定高 zoom 影像被贴到不同 surface 尺度后产生清晰度块状差异。

## 仍未完成的 OpenGlobus 行为级对齐

- SurfaceTile terrain 已按 target tile key 查找 exact terrain tile，并沿父链查找 parent fallback terrain tile；parent fallback 已按父 tile bounds 裁剪采样；已用 skirt 作为接缝降级策略。尚未实现完整邻接 seam stitching/edge height reconciliation，因为还缺少跨 tile 邻接 terrain registry 与边高程同步接口。
- normal map 已实现派生纹理、上传和 shader 采样；与 OpenGlobus 的差异是当前在 CPU 侧编码 RGBA8 后上传，而 OpenGlobus 使用 `NormalMapCreator` framebuffer pass 把 normal 数组渲染成 texture，并可选 blur。后续若启用 terrain normal map，应继续演进为独立队列/每帧预算消费，而不是在 `SurfaceTileCommand` 构建路径里无条件同步创建。
- high/low split 已在 CPU SurfaceVertex 契约中落地；GPU vertex attribute / shader 侧 high-low 上传仍待 `shader-interface.md` 对应扩展。
- OpenGlobus Earth 三分区 scheme 已实现编码、bounds、极区 surface 采样、provider compatibility 和 grouped-y URL 映射；debug overlay 文本和更细的 tile availability 区域/层级矩阵仍待完善。
- LOD 已有 projected-size 与节点统计，LayerPlan 已有 transition/readiness 状态；尚未完整实现 cesium-native 风格 selection state、Kicking、Ancestor Meets SSE、loadingDescendantLimit、raster target pixels，也尚未完整实现 OpenGlobus rendered nodes 邻接事件、terrain readiness observer 和 min/max visible zoom 事件。

## 验收要求

- 单元测试必须覆盖 WGS84 往返、极区投影、ray miss/hit、Vincenty 距离、三分区 tile group、LOD 诊断和 terrain mesh 高度采样。
- 所有新增接口必须明确单位、CRS、坐标顺序和 miss/invalid 语义。
- 后续若新增地形渲染能力，必须继续通过 `SurfaceTile` 主链路，不能恢复独立单位球地形 surface pass。
