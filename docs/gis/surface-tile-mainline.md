# SurfaceTile 主链路

本文件定义 MVP / 标准底图 / 3D globe 的正式主链路。它用于替代“先画 globe，再把 basemap tile 作为另一层共面 mesh 贴上去”的过渡方案。

## 强制结论

标准底图不得作为独立共面 `BasemapTileCommand` 复用主链路。正式主链路必须采用：

```text
FrameState
-> TilePlan / SurfaceTilePlan
-> SurfaceTile geometry
-> ImageryAttachment
-> SurfaceTileCommand
-> RenderDevice
```

核心原则：

- `SurfaceTile` 是地球表面几何和 depth 的唯一来源。
- `ImageryLayer` 只提供纹理 attachment，不创建另一套共面地表几何。
- `SurfaceTileCommand` 同时绑定 surface mesh、imagery textures、fallback UV window 和固定 render state。
- `globe` 只允许作为无瓦片 fallback、loading background 或低级别占位，不得与标准底图瓦片在同一表面长期竞争 depth。
- 地形接入后，`SurfaceTile` 的几何来源从 ellipsoid surface 切换到 terrain surface；imagery 仍然作为 attachment 贴覆到同一个 surface mesh。

## 禁止的过渡方案

以下方案不得作为 MVP 标准底图主链路继续扩展：

```text
GlobeCommand depth/color
+ BasemapTileCommand 独立共面 mesh
```

原因：

- depth 来源不唯一，天然产生共面 z-fighting。
- globe mesh 与 tile mesh tessellation 不一致，边界、winding、精度和遮挡难以稳定。
- `depthTest`、`cullFace`、polygon offset 容易退化成视觉补丁。
- terrain 接入后必须推翻，拖慢长期节奏。
- 可见性、LOD、fallback、picking 和 diagnostics 被拆散在多个临时链路里。

允许的例外：

- 调试 overlay 可以独立 draw，但必须标为 debug pass，且不得作为正常底图正确性的依据。
- 临时迁移期可以保留旧 `BasemapTileCommand` 代码，但必须标记为 deprecated，不得新增功能，不得作为验收路径。

## 核心数据结构

```text
SurfaceTileKey {
  tileKey: TileKey
  surfaceProfile: "ellipsoid" | "terrain"
  terrainVersion?
  timeKey?
}

SurfaceTile {
  key: SurfaceTileKey
  bounds: Rectangle              // radian, WGS84 cartographic extent
  mesh: SurfaceTileMesh
  boundingVolume: BoundingVolume
  visibility: SurfaceVisibility
  imageryAttachments: ImageryAttachment[]
  state: missing | building | ready | failed | evicted
  generation: uint64
}

SurfaceTileMesh {
  vertices: SurfaceVertex[]      // CPU authoritative ECEF in meter
  indices: uint32[]
  vertexLayout: SurfaceVertexLayout
  winding: outward
  sampling: TileSurfaceSampling
}

SurfaceVertex {
  positionEcef: Cartesian3       // double on CPU
  normalEcef: Cartesian3
  mercatorUv: [u, v]             // Web Mercator imagery UV, top-left origin
  cartographic: Cartographic     // optional debug/diagnostic source
}

ImageryAttachment {
  layerId: string
  providerId: string
  textureKey: TileKey
  texture: TextureRef
  uvWindow: [offsetU, offsetV, scaleU, scaleV]
  opacity: number
  colorSpace: "sRGB" | "linear"
  fallbackSource: "exact" | "parent" | "placeholder"
}

SurfaceTileCommand {
  kind: "SurfaceTile"
  pass: "color"
  mesh: SurfaceTileMeshRef
  imageryAttachments: ImageryAttachmentRef[]
  renderState: SurfaceRenderState
  boundingVolume: BoundingVolume
  frameId: uint64
}
```

## 贴地采样规则

标准 XYZ / TMS Web Mercator 底图的 surface mesh 必须按 Web Mercator 网格采样：

```text
u -> longitude linear
v -> WebMercatorY linear
WebMercatorY -> geodetic latitude
longitude/latitude/height -> WGS84 ECEF
```

禁止：

- 将 Web Mercator tile 的 `v` 直接线性映射到 geodetic latitude。
- 在 shader 中临时修 CRS 或 provider 偏移。
- 用高度抬升来掩盖共面或接缝问题。

验收：

- `TileSurface` 单元测试覆盖 north-west / south-east UV 方向。
- 顶点 `cartographic -> ECEF -> cartographic` 后高度接近 0。
- 三角形 winding 从地球外侧观察为 outward，可稳定开启 backface culling。
- parent fallback 的 `uvWindow` 按 Mercator Y 计算，而不是按纬度差计算。

## 渲染状态

标准 `SurfaceTileCommand` 的固定状态：

```text
depthTest  = true
depthWrite = true
cullFace   = true
blend      = false when only one opaque base imagery layer
blend      = true  when multi-imagery opacity or alpha requires it
```

说明：

- `SurfaceTile` 自身写 depth，因为它就是地球表面。
- imagery attachment 不再作为独立共面 mesh，因此不需要用独立 tile pass 与 globe depth 竞争。
- 多影像图层混合发生在 `SurfaceTileCommand` 内部或受控的 surface material pass 内，不得新建另一套共面几何。

## 每帧主流程

```text
1. Build FrameState
2. SurfaceTilePlanBuilder selects visible surface tiles
3. SurfaceTileBuilder creates or reuses ellipsoid/terrain mesh
4. ImageryLayerPlan selects exact texture or parent fallback for each surface tile
5. TextureUploader uploads ready decoded imagery within per-frame budget
6. SurfaceRenderQueue emits SurfaceTileCommand only for current frame/generation
7. Renderer validates fixed MVP surface pass order and state
8. DebugOverlay consumes diagnostics only after command generation
```

## 与 TilePlan 的关系

`TilePlan` 仍负责共享可见 tile 候选、LOD 和 request/render 分离，但最终渲染对象必须升级为 `SurfaceTile`：

```text
visibleTiles
-> desiredSurfaceTiles
-> requestImageryTiles
-> ready ImageryAttachment
-> renderSurfaceTiles
```

`renderSurfaceTiles` 才能生成 `SurfaceTileCommand`。网络回调、图片解码和纹理上传只能更新资源状态，不能直接提交 draw command。

## 地形演进

MVP 可以先用 ellipsoid `SurfaceTileMesh`：

```text
SurfaceTile(surfaceProfile = "ellipsoid")
```

地形接入后改为：

```text
SurfaceTile(surfaceProfile = "terrain")
```

影像贴覆规则不变。变化的是 surface mesh 的高度、法线、裙边、接缝和 bounding volume。不得重新引入独立 imagery mesh。

## 迁移要求

重构当前实现时按以下顺序推进：

1. 新增 `SurfaceTileMesh`，复用并扩展 `TileSurface` 的 Mercator-v 采样测试。
2. 新增 `SurfaceTile` 和 `ImageryAttachment`，把 texture cache 命中结果绑定为 attachment。
3. 新增 `SurfaceTileCommand`，替代标准底图 `BasemapTileCommand`。
4. `Renderer` 增加 surface tile shader / pipeline，或把现有 tile shader 改造成 surface material shader。
5. `Scene` 默认提交 `SurfaceTileCommand`；globe command 只作为 no-imagery fallback。
6. 删除或 deprecated 独立 basemap tile 主链路。

## 验收清单

- 首屏地球表面由 `SurfaceTileCommand` 渲染，不是 `GlobeCommand + BasemapTileCommand`。
- Android GL ES 和 iOS Metal 都能显示标准 XYZ Web Mercator 底图。
- 背面 surface tile 不进入 render queue。
- `SurfaceTileCommand` depth/cull/blend 状态由固定验证器检查。
- parent fallback UV window 正确，且 Web Mercator 高纬不会错位。
- 快速旋转和缩放不会出现过期瓦片污染当前帧。
- diagnostics 能输出 visible surface tiles、render surface tiles、imagery attachment state、texture count 和 generation。
