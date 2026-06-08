# 底图瓦片渲染编排

底图瓦片渲染是地球引擎的基础流水线。它不是简单地把 `z/x/y` 图片贴到球上，而是从相机状态、瓦片体系、图层栈、LOD、请求调度、缓存、纹理上传、父子瓦片替换、多图层混合到失败降级的一整套编排。

本文件主要面向影像瓦片、电子地图瓦片、栅格底图、WMTS/XYZ/TMS/私有底图。地形瓦片和 3D Tiles 另见 `tiles-terrain-lod.md` 和 `three-d-tiles.md`，但调度思想相通。

MVP / 标准底图 / 3D globe 的正式渲染主链路必须遵守 `surface-tile-mainline.md`：影像瓦片作为 `SurfaceTile` 的 imagery attachment 渲染，不作为独立共面 `BasemapTileCommand`。

## 核心目标

底图瓦片渲染要同时满足：

- 空间正确：瓦片位置、CRS、tile scheme 无偏移。
- 视觉连续：缩放、旋转、切换时不闪白、不大面积跳变。
- 渐进加载：高层级未就绪时可用父瓦片或低清瓦片占位。
- 可取消：相机移动后旧请求不污染新视图。
- 可组合：多底图、多图层、透明度、mask、色彩调整可编排。
- 可观测：能看到 tile 状态、请求队列、缓存命中、纹理数量。

## 推荐模块

```text
FrameState
  -> BasemapLayerStack
  -> TilePlanBuilder
  -> TileVisibilitySelector
  -> TileRequestScheduler
  -> TileCache
  -> TileDecoder
  -> TextureUploader
  -> SurfaceTileBuilder
  -> ImageryAttachmentBuilder
  -> SurfaceRenderQueue
  -> Renderer
```

职责边界：

- `FrameState`：相机、视口、时间、屏幕密度、渲染模式。
- `BasemapLayerStack`：底图图层顺序、透明度、可见范围、混合模式。
- `TilePlanBuilder`：按 tile scheme、CRS profile、图层组和时间生成本帧 tile plan。
- `TileVisibilitySelector`：选出当前帧需要的 tile key。
- `TileRequestScheduler`：请求优先级、并发、取消、重试。
- `TileCache`：raw、decoded、texture 多级缓存。
- `TileDecoder`：图片解码、格式检查、色彩空间处理。
- `TextureUploader`：把 decoded image 上传为 GPU texture。
- `SurfaceTileBuilder`：生成或复用地球表面 mesh，ellipsoid MVP 使用 Web Mercator-v 采样到 WGS84 ECEF。
- `ImageryAttachmentBuilder`：为每个 surface tile 绑定 exact texture、parent fallback 或透明缺失策略。
- `SurfaceRenderQueue`：决定本帧画哪些 `SurfaceTileCommand`。
- `Renderer`：按 render order 和 blend state 执行 draw。

## 每帧编排

每一帧或每次 requestRender 应按固定流程：

1. 读取 `FrameState`。
2. 根据相机和地球/地形可见区域计算可见 tile set。
3. 对每个底图图层计算目标 LOD。
4. 生成 `desiredTiles`。
5. 查询 cache，分成 ready、loading、missing、failed。
6. 对 missing tile 入请求队列。
7. 对过期或不可见请求降级或取消。
8. 为 ready surface tile 绑定 imagery attachment。
9. 对未 ready imagery 查找可用 parent/ancestor fallback，并计算 Mercator UV window。
10. 对 `renderSurfaceTiles` 生成 `SurfaceTileCommand`。
11. 更新统计和 debug overlay。

不要让网络回调直接修改最终可见集。网络回调只能更新 tile 状态，最终画什么由当前帧的 render queue 决定。

## Tile Plan 策略

瓦片策略不能简单理解为“每个数据源都完整计算一遍”，也不能把所有图层强行共用同一套 tile set。推荐使用 `TilePlan` 分层编排。

```text
TilePlan {
  frameId
  schemeId
  crsProfile
  timeKey?
  visibleTiles: TileKey[]
  desiredSurfaceTiles: SurfaceTileKey[]
  layerPlans: LayerTilePlan[]
}

LayerTilePlan {
  layerId
  providerId
  desiredTiles: TileKey[]
  imageryAttachments: ImageryAttachment[]
  requestTiles: TileKey[]
  fallbackTiles: TileFallback[]
}
```

### 共享计算

以下计算应尽量按 `TileScheme + CrsProfile + FrameState` 共享：

- 相机视域与地球相交。
- horizon / frustum 可见性。
- tile bounds 与视域相交。
- 屏幕空间覆盖面积。
- 基础目标 LOD。
- 反经线、极区和 provider bounds 的几何裁剪。

例如多个图层都使用标准 XYZ Web Mercator，且 zoom/resolution 策略一致，可以共享同一个 `visibleTiles` 基础集合。

### 图层独立计算

以下内容必须按图层独立计算：

- provider availability。
- minZoom / maxZoom。
- 图层可见范围和 opacity。
- 图层时间片。
- style/version 是否影响 cache key。
- 失败、重试、权限、token。
- attribution。
- 请求优先级权重。
- 是否允许 parent fallback。
- 是否允许预取。

同一个 tile key 在不同图层中通常对应不同 URL、不同缓存、不同纹理和不同 attribution，不能只因为 `z/x/y` 一样就混用资源。

### 分组策略

推荐把图层按以下 key 分组：

```text
TileGroupKey = {
  tileSchemeId
  crsProfile
  matrixSet
  tileSize
  timeKey?
}
```

同组图层共享可见 tile 候选集；不同组分别计算。例如：

- OSM XYZ 与某个标准 XYZ 注记层可以同组。
- WMTS EPSG:4326 geographic tiling 与 XYZ Web Mercator 不能同组。
- 高德/腾讯 GCJ-02 体系不能和 WGS84 业务瓦片直接同组。
- 百度瓦片应独立 group。
- 时序影像即使 tile scheme 相同，也需要按 timeKey 派生 layer plan。

### 请求与渲染分离

`desiredTiles` 不等于 `requestTiles`，也不等于 `renderSurfaceTiles`：

- `desiredTiles`：当前图层理想情况下需要、且 provider 覆盖范围/可用矩阵支持的 tile。
- `unsupportedTiles`：当前 TilePlan 可见、但 provider 不支持的 tile；不得进入 `desiredTiles`、`requestTiles` 或 `renderSurfaceTiles`，也不得计为 imagery missing。
- `requestTiles`：cache 缺失且允许请求的 tile。
- `ImageryAttachment`：本帧可绑定到 surface tile 的纹理，可能是目标 tile，也可能是 parent fallback。
- `renderSurfaceTiles`：本帧实际可画的地表 tile，必须同时拥有有效 surface mesh 和 imagery attachment 策略。

这样可以避免网络、缓存和渲染互相污染。

### 计算频率

不需要每帧对所有数据源重算全量 tile plan。可以按 dirty flag 增量更新：

- 相机变化：重算可见 tile 和 LOD。
- 图层显隐/顺序/透明度变化：重算 layer plan 和 render queue。
- provider availability 变化：重算受影响图层。
- 时间轴变化：重算带 timeKey 的图层。
- 样式版本变化：重算 cache key 和 render resource。
- 网络返回：只更新 tile 状态，并触发 render queue 重新选择。

但每次渲染前必须保证 render queue 来自当前最新 frame state，不能使用过期相机状态直接绘制。

### 叠加层与底图区别

底图瓦片通常要求连续铺满视域，因此 fallback 和预取更重要。专题叠加层可以按业务允许：

- 允许空洞。
- 只显示有数据区域。
- 不使用 parent fallback。
- 使用透明 missing tile。
- 降低请求优先级。

这意味着底图和叠加层可以共享 `visibleTiles`，但不应共享同一套 fallback 和请求策略。

## 可见 Tile 选择

可见选择要考虑：

- frustum 与地球/椭球相交。
- horizon culling。
- tile bounds 与视域相交。
- screen pixel error 或目标分辨率。
- minZoom/maxZoom。
- 图层 availability。
- 极区、反经线、provider bounds。

对于 3D globe，不能直接套 2D viewport bbox 选瓦片。需要根据相机、椭球、地平线和 tile bounds 做保守可见判断。

## LOD 策略

底图 LOD 可用：

- 屏幕像素密度。
- tile 原始分辨率。
- 相机高度。
- 地表点到相机距离。
- provider zoom 限制。
- DPR。

LOD 必须有滞回 hysteresis，避免相机轻微缩放时 z/z+1 来回抖动。

## 父子瓦片替换

高层级子瓦片未就绪时，应优先使用父瓦片 fallback：

- 子瓦片 ready 后再替换父瓦片。
- 四个子瓦片可设置 all-ready 替换，避免四分之一高清、四分之三低清造成明显拼接。
- 或使用渐进混合 crossfade，但要控制性能。
- 父瓦片纹理坐标必须裁剪到子瓦片对应区域。
- Web Mercator imagery 的 V 方向必须按 Mercator Y 计算，不能按 geodetic latitude 差值计算。

禁止在子瓦片未 ready 时直接留白，除非当前图层明确允许空白。

## SurfaceTile 渲染

标准底图最终渲染对象是 `SurfaceTileCommand`，不是独立 imagery mesh。

```text
SurfaceTile {
  tileKey
  bounds
  mesh
  boundingVolume
  imageryAttachments
  generation
}
```

MVP ellipsoid surface mesh 的采样规则：

```text
u -> longitude linear
v -> WebMercatorY linear
WebMercatorY -> latitude
longitude/latitude/height0 -> WGS84 ECEF
```

固定要求：

- `SurfaceTile` 写 depth；imagery attachment 只是材质输入。
- `GlobeCommand + BasemapTileCommand` 不得作为标准底图验收链路。
- shader 与 CPU `TileSurface` 测试必须使用同一套 Mercator-v 采样语义。
- surface tile mesh winding 必须 outward，保持 `cullFace=true`。
- 过期 generation 的 surface 或 imagery attachment 不得进入当前帧 `SurfaceRenderQueue`。

## 请求优先级

请求优先级建议综合：

- 当前帧可见优先。
- 屏幕中心优先。
- 面积占比大优先。
- 当前 LOD 优先，预取次级。
- 用户交互停止后的精化优先。
- 高优先级图层优先，例如主底图高于可选叠加底图。

请求队列必须支持取消或过期检查。旧相机状态下的请求返回后，不能直接覆盖当前 tile 状态。

## 缓存层级

建议分层：

- `RawCache`：原始字节缓冲区（网络 response body）。
- `DecodedCache`：平台解码后的像素缓冲区（iOS: CGImage 解码输出、Android: Bitmap 解码输出、stb_image 回退）。
- `TextureCache`：GPU texture（通过 RenderDevice 创建）。
- `RenderTileCache`：tile mesh/uv/command。

缓存 key 必须包含：

- provider id。
- layer id。
- tile key。
- style/version。
- tile scale 或屏幕密度等级（@1x/@2x/@3x），如果影响资源。
- time，如果是时序底图。

不要用裸 `z/x/y` 作为全局缓存 key。

## 纹理上传

纹理上传策略：

- 优先使用平台原生图片解码器（iOS: CGImage、Android: BitmapFactory），在后台线程解码为原始像素缓冲区，主线程仅做 GPU 上传。
- 对于不依赖平台的线程池解码，使用 stb_image 或 libjpeg-turbo。
- 限制每帧上传纹理数量，避免掉帧（移动端建议 ≤ 4 张/帧）。
- 对大图或 512/1024 tile 估算显存。
- 明确 mipmap、filter、wrap、color space。
- context lost 或 resource eviction 后可恢复或重新请求和上传。

纹理上传是常见卡顿点，不应在一帧内无限上传所有 ready 图片。

## 多底图图层栈

底图图层栈应支持：

- base layer：主底图，通常不透明。
- overlay imagery：透明叠加，例如路网、注记、专题影像。
- mask/clip：按区域裁剪。
- color adjustment：亮度、对比度、饱和度、gamma。
- blend mode：normal、multiply、screen 等，按项目需要谨慎开放。

图层顺序必须稳定。不同 CRS/tile scheme 的底图叠加必须先通过 `multi-tile-schemes.md` 验证无偏移。

## 栅格重投影与重采样

如果底图 CRS 与引擎统一空间不同：

- 优先在 tile bounds 和顶点层处理空间定位。
- 必要时使用 shader 或离屏 pass 做重投影，但必须说明误差。
- 不要把百度墨卡托、GCJ-02 或私有投影伪装成 EPSG:3857。
- 重采样方式 nearest、linear、cubic 会影响视觉和科学数据含义。

科学栅格和业务栅格不能随意做视觉重采样后用于分析。

## 失败与降级

失败策略必须区分：

- 404：瓦片不存在。
- 401/403：权限或 token。
- timeout：网络慢。
- decode error：图片损坏或格式不支持。
- network unavailable：无网络连接（移动端 WiFi↔蜂窝切换常见）。
- provider out of bounds：超出覆盖范围。

降级方式：

- 使用 parent fallback。
- 显示透明 tile。
- 显示错误占位，只在 debug 模式。
- 重试有限次数。
- 暂停该 provider 并提示 attribution/权限问题。

不要无限重试失败瓦片。

## 预取策略

可选预取：

- 当前视域外一圈邻居 tile。
- 相机运动方向上的 tile。
- 当前 zoom 的 parent 或 next zoom。
- 用户停止交互后再精化。

预取不能挤占当前可见 tile 的请求带宽。

## 与地形的关系

底图可以：

- 贴在椭球体上。
- 贴在 terrain mesh 上。
- 作为 terrain tile 的 imagery layer。

必须明确：

- 影像 tile 和 terrain tile 的 LOD 是否独立。
- 影像投影到地形时如何处理 UV。
- terrain tile 缺失时是否回退到椭球。
- 影像和地形边界是否可能错位。

## Render Command 排序

推荐顺序：

1. globe depth / terrain depth。
2. base imagery。
3. additional imagery overlays。
4. raster thematic layers。
5. vector overlays。
6. 3D Tiles / models，按场景策略。
7. labels / billboards。
8. atmosphere/weather/postprocess。

实际顺序要结合透明、depth、classification 和后处理，不能只靠数组顺序硬凑。

## Debug Overlay

底图瓦片必须支持 debug：

- tile boundary。
- z/x/y/provider/layer。
- state：missing、queued、loading、decoded、texture-ready、rendered、failed。
- request priority。
- cache hit/miss。
- parent fallback。
- texture memory estimate。
- LOD error。
- provider bounds。

没有 debug overlay 时，瓦片错位、闪烁和白屏很难定位。

## 验收清单

底图瓦片渲染实现后至少验证：

- 首屏不白屏。
- 快速缩放和平移不出现大面积空洞。
- 子瓦片未 ready 时父瓦片能正确裁剪 fallback。
- 切换底图图层不遗留旧纹理。
- 多底图叠加无系统性偏移。
- 失败瓦片不会无限重试。
- 请求队列能取消过期请求。
- 每帧纹理上传数量受控。
- 图层隐藏后请求和 GPU 资源按策略释放或保留。
- debug overlay 能显示 tile 状态。
