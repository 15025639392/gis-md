# 地球引擎参考架构

本文件定义从 0 开发地球引擎时推荐的模块和目录结构。具体技术栈可以变化，但职责边界应保持清晰。

## 推荐目录

```text
src/
  core/
    math/
    geodesy/
    time/
    events/
  scene/
    Scene.ts
    FrameState.ts
    Primitive.ts
    Layer.ts
  renderer/
    Renderer.ts
    RenderCommand.ts
    ShaderProgram.ts
    Texture.ts
    Buffer.ts
    Framebuffer.ts
    passes/
  globe/
    Globe.ts
    EllipsoidMesh.ts
    TerrainSurface.ts
  camera/
    Camera.ts
    CameraController.ts
  tiling/
    TileScheme.ts
    TileKey.ts
    TilePlan.ts
    TileCache.ts
  providers/
    ImageryProvider.ts
    TerrainProvider.ts
    VectorProvider.ts
    TilesetProvider.ts
  layers/
    BasemapLayer.ts
    TerrainLayer.ts
    VectorLayer.ts
    TilesetLayer.ts
    AnnotationLayer.ts
  styling/
    OverlayStyle.ts
    StyleExpression.ts
  interaction/
    InputManager.ts
    PickingService.ts
    SelectionManager.ts
    EditingManager.ts
    MeasurementTool.ts
  environment/
    Atmosphere.ts
    Sky.ts
    Lighting.ts
    Weather.ts
  debug/
    DebugOverlay.ts
    Diagnostics.ts
  workers/
    tileDecode.worker.ts
  examples/
```

## 依赖方向

允许：

- `layers` 依赖 `providers`、`tiling`、`renderer` 抽象。
- `renderer` 依赖 `core/math`，不依赖业务 provider。
- `interaction` 通过 public API 操作 scene/layers，不直接改 GPU 资源。
- `providers` 负责数据请求和解析，不创建长期 GPU 资源。

禁止：

- `provider` 直接调用 WebGL/WebGPU。
- `shader` 处理 CRS 偏移或业务权限。
- `renderer` 发网络请求。
- `UI` 直接操作 tile cache 内部结构。
- `layer` 私自维护与全局冲突的 selection 状态。

## 运行时主循环

```text
Input/Event
  -> update Scene state
  -> build FrameState
  -> update TilePlans / Layers
  -> schedule requests and uploads
  -> build RenderCommands
  -> execute RenderPasses
  -> update Diagnostics
```

网络请求和 worker 解析只更新资源状态；当前帧最终渲染什么由 FrameState 和 RenderQueue 决定。

## Public API 示例

```text
Engine
  create(container, options)
  destroy()
  render()
  requestRender()
  addLayer(layer)
  removeLayer(layerId)
  getCamera()
  flyTo(target, options)
  pick(screen)
  setTime(time)
  getDiagnostics()
```

API 不应暴露内部 WebGL 对象、tile cache map 或 provider 私有状态。

## Layer 生命周期

```text
created
  -> added
  -> loading
  -> ready
  -> visible/hidden
  -> error
  -> removed
  -> destroyed
```

Layer 必须支持：

- `update(frameState)`
- `buildRenderCommands(frameState)`
- `setVisible(boolean)`
- `dispose()`
- `getAttribution()`
- `getDiagnostics()`

## Provider 生命周期

```text
created
  -> metadata-loading
  -> ready
  -> requesting
  -> failed
  -> disposed
```

Provider 不应知道具体 UI，不应长期持有 renderer 资源。

## Renderer 职责

Renderer 负责：

- 管理 GPU resource。
- 编译 shader/pipeline。
- 执行 render pass。
- 管理 framebuffer。
- 提供 capability。
- 处理 context/device lost。
- 提供 GPU 资源统计。

Renderer 不负责：

- 选择业务图层。
- 计算 provider URL。
- 判断用户权限。
- 做 CRS 纠偏。

## 调试架构

调试能力必须是架构一部分：

- FrameState inspector。
- TilePlan inspector。
- Layer state inspector。
- GPU resource table。
- Request queue。
- Picking inspector。
- Camera inspector。
- Performance timeline。

不要等引擎复杂后再补 debug。
