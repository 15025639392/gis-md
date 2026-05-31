# 核心 API 契约

本文件定义从 0 开发地球引擎时必须稳定下来的核心接口。语言不限，但概念和边界应保持。

## 基础类型

```text
Cartographic {
  longitude: number
  latitude: number
  height: number
  unit: "radian" | "degree"
}

Cartesian3 {
  x: number
  y: number
  z: number
  unit: "meter"
}

Rectangle {
  west: number
  south: number
  east: number
  north: number
  unit: "radian" | "degree"
}
```

内部推荐使用 radian，外部 API 可以接收 degree，但必须命名清楚。

## Ellipsoid

```text
Ellipsoid {
  semiMajorAxis: number
  semiMinorAxis: number
  flattening: number

  cartographicToCartesian(input): Cartesian3
  cartesianToCartographic(input): Cartographic
  geodeticSurfaceNormalCartographic(input): Cartesian3
  scaleToGeodeticSurface(input): Cartesian3
}
```

必须提供 WGS84 默认实例。

## Camera

```text
Camera {
  position: Cartesian3
  direction: Cartesian3
  up: Cartesian3
  right: Cartesian3
  frustum: Frustum

  setView(options)
  lookAt(target, offset)
  flyTo(target, options)
  getPickRay(screen): Ray
}
```

Camera 不应直接请求瓦片或修改图层数据。

## FrameState

```text
FrameState {
  frameId: number
  time: number
  camera: Camera
  viewport: { width, height, dpr }
  mode: "3D" | "2D" | "Columbus"
  passes: RenderPassFlags
  diagnostics: Diagnostics
}
```

FrameState 是每帧计算的唯一上下文来源。

## TileScheme

```text
TileScheme {
  id: string
  crsProfile: string
  tileSize: number
  minZoom: number
  maxZoom: number

  tileToRectangle(key): Rectangle
  positionToTile(position, zoom): TileKey
  getLevelResolution(zoom): number
  getTileRange(rectangle, zoom): TileRange
}
```

TileScheme 不负责请求 URL。

## TileKey

```text
TileKey {
  schemeId: string
  z: number
  x: number
  y: number
  time?: string
}
```

缓存 key 必须额外包含 provider/layer/style/version，不得只用 TileKey。

## Provider

```text
Provider<T> {
  id: string
  type: string
  metadata: ProviderMetadata
  availability: Availability

  loadMetadata(signal): Promise<ProviderMetadata>
  request(key, signal): Promise<ArrayBuffer | Blob | Response>
  parse(raw): Promise<T>
  dispose(): void
}
```

Provider 必须支持取消。

## Layer

```text
Layer {
  id: string
  type: string
  visible: boolean
  opacity: number

  update(frameState): void
  buildRenderCommands(frameState, commandList): void
  pick(context): PickResult[]
  getAttribution(): Attribution[]
  dispose(): void
}
```

Layer 是 provider data 到 render command 的桥。

## RenderCommand

```text
RenderCommand {
  owner: string
  pass: "depth" | "color" | "picking" | "shadow" | "postprocess"
  pipeline: PipelineRef
  vertexArray: VertexArrayRef
  uniforms: object
  textures: TextureRef[]
  renderState: RenderState
  boundingVolume?: BoundingVolume
}
```

RenderCommand 不应包含网络请求或业务权限判断。

## Picking

```text
PickResult {
  hitType: string
  screen: [number, number]
  cartographic?: Cartographic
  worldPosition?: Cartesian3
  layerId?: string
  featureId?: string
  objectId?: string
  metadata?: object
}
```

Picking 输出必须说明坐标 CRS 和高度来源。

## Diagnostics

```text
Diagnostics {
  fps
  frameTime
  drawCalls
  visibleTiles
  queuedRequests
  loadingRequests
  textureCount
  bufferCount
  gpuMemoryEstimate
}
```

Diagnostics 是开发和验收的一等 API，不是临时 console.log。
