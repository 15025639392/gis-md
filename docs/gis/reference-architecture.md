# 地球引擎参考架构

本文件定义从 0 开发 C++ 移动端地球引擎时推荐的模块和目录结构。具体技术栈以 `technology-decisions.md` 为准，但职责边界应保持清晰。

## 推荐目录

```text
src/
  core/
    math/
      Vec3.h / Vec3.cpp
      Mat4.h / Mat4.cpp
      Ray.h / Ray.cpp
      Rectangle.h / Rectangle.cpp
    geodesy/
      Ellipsoid.h / Ellipsoid.cpp
      Cartographic.h / Cartographic.cpp
      Transforms.h / Transforms.cpp
    time/
      TimeController.h / TimeController.cpp
    events/
      EventBus.h / EventBus.cpp
      Command.h / Command.cpp
  scene/
    Scene.h / Scene.cpp
    FrameState.h / FrameState.cpp
    Primitive.h / Primitive.cpp
    Layer.h / Layer.cpp
  renderer/
    RenderDevice.h              // 平台抽象：Metal / GL ES / Vulkan
    Renderer.h / Renderer.cpp
    RenderCommand.h / RenderCommand.cpp
    ShaderProgram.h / ShaderProgram.cpp
    Texture.h / Texture.cpp
    Buffer.h / Buffer.cpp
    Framebuffer.h / Framebuffer.cpp
    passes/
  globe/
    Globe.h / Globe.cpp
    EllipsoidMesh.h / EllipsoidMesh.cpp
    TerrainSurface.h / TerrainSurface.cpp
  camera/
    Camera.h / Camera.cpp
    CameraController.h / CameraController.cpp
  tiling/
    TileScheme.h / TileScheme.cpp
    TileKey.h / TileKey.cpp
    TilePlan.h / TilePlan.cpp
    TileCache.h / TileCache.cpp
  providers/
    ImageryProvider.h / ImageryProvider.cpp
    TerrainProvider.h / TerrainProvider.cpp
    VectorProvider.h / VectorProvider.cpp
    TilesetProvider.h / TilesetProvider.cpp
  layers/
    BasemapLayer.h / BasemapLayer.cpp
    TerrainLayer.h / TerrainLayer.cpp
    VectorLayer.h / VectorLayer.cpp
    TilesetLayer.h / TilesetLayer.cpp
    AnnotationLayer.h / AnnotationLayer.cpp
  styling/
    OverlayStyle.h / OverlayStyle.cpp
    StyleExpression.h / StyleExpression.cpp
  interaction/
    InputManager.h / InputManager.cpp
    GestureRecognizer.h / GestureRecognizer.cpp
    PickingService.h / PickingService.cpp
    SelectionManager.h / SelectionManager.cpp
    EditingManager.h / EditingManager.cpp
    MeasurementTool.h / MeasurementTool.cpp
  environment/
    Atmosphere.h / Atmosphere.cpp
    Sky.h / Sky.cpp
    Lighting.h / Lighting.cpp
    Weather.h / Weather.cpp
  threading/
    TaskThreadPool.h / TaskThreadPool.cpp
    TileDecodeTask.h / TileDecodeTask.cpp
  platform/
    ios/
      RenderDeviceMetal.h / RenderDeviceMetal.mm
      PlatformBridge.mm
      InputProviderIOS.mm
    android/
      RenderDeviceGLES.h / RenderDeviceGLES.cpp
      RenderDeviceVulkan.h / RenderDeviceVulkan.cpp
      PlatformBridge.cpp
      InputProviderAndroid.cpp
    bridge/
      PlatformBridge.h           // 平台注入接口（网络、文件、日志、系统信号）
      HttpRequest.h / HttpRequest.cpp
      FileSystem.h / FileSystem.cpp
  debug/
    DebugOverlay.h / DebugOverlay.cpp
    Diagnostics.h / Diagnostics.cpp

examples/
  ios/
    MinimalGlobe/              // Xcode project 或 CMake 生成
  android/
    MinimalGlobe/              // Gradle project + native lib

tests/
  unit/
    core/                      // GoogleTest 单元测试
    geodesy/
    tiling/
  integration/
  fixtures/                    // 测试数据
```

## CMake 布局

```text
earth-engine/
  CMakeLists.txt               // 顶层：project + add_subdirectory
  src/
    CMakeLists.txt             // earth_engine_core 静态库
  examples/
    ios/
      CMakeLists.txt           // iOS app target (Xcode generator)
    android/
      CMakeLists.txt           // Android NDK lib target
  tests/
    CMakeLists.txt             // GoogleTest targets
  vcpkg.json                   // 依赖声明
  cmake/
    FindGLM.cmake              // 自定义 find module（如需要）
    Toolchain-ios.cmake        // iOS cross-compilation
    Toolchain-android.cmake    // Android cross-compilation
```

## 依赖方向

允许：

- `layers` 依赖 `providers`、`tiling`、`renderer` 抽象（RenderDevice 接口，不依赖具体实现）。
- `renderer` 依赖 `core/math`，通过 `platform/bridge/PlatformBridge.h` 获取平台能力，不直接 `#import <Metal/Metal.h>` 或 `#include <GLES3/gl3.h>`。
- `interaction` 通过 public API 操作 scene/layers，不直接改 GPU 资源。
- `providers` 负责数据请求和解析，不创建长期 GPU 资源。
- `threading` 只依赖 `core` 和 `providers` 的解析接口，不依赖 `renderer`。
- `platform/` 子目录可以引入平台原生 API，但必须通过 `platform/bridge/` 中的抽象接口暴露给引擎核心。

禁止：

- `provider` 直接调用 Metal/GL/Vulkan API。
- `shader` 处理 CRS 偏移或业务权限。
- `renderer` 发网络请求（网络通过 `platform/bridge/HttpRequest` 注入）。
- `layer` 私自维护与全局冲突的 selection 状态。
- `core/math` 依赖 `platform/`。

## 渲染设备抽象 (RenderDevice)

RenderDevice 是引擎核心与平台 GPU 的唯一桥梁：

```cpp
// src/renderer/RenderDevice.h
class RenderDevice {
public:
    virtual ~RenderDevice() = default;

    // 能力查询
    virtual int maxTextureSize() const = 0;
    virtual int maxDrawBuffers() const = 0;
    virtual bool supportsFloatTextures() const = 0;
    virtual std::string rendererString() const = 0;

    // 资源创建（返回 opaque handles 或 RAII 对象）
    virtual std::unique_ptr<Texture> createTexture(const TextureDesc&) = 0;
    virtual std::unique_ptr<Buffer> createBuffer(const BufferDesc&) = 0;
    virtual std::unique_ptr<ShaderProgram> createShader(const ShaderDesc&) = 0;
    virtual std::unique_ptr<Framebuffer> createFramebuffer(const FramebufferDesc&) = 0;

    // 帧操作
    virtual void beginFrame() = 0;
    virtual void submit(const RenderCommandList&) = 0;
    virtual void endFrame() = 0;

    // 生命周期
    virtual void onSurfaceCreated() = 0;   // 首次创建或 context lost 后重建
    virtual void onSurfaceChanged(int w, int h) = 0;
    virtual void onSurfaceDestroyed() = 0; // 释放所有 GPU 资源
};
```

平台实现：

- iOS: `RenderDeviceMetal` 封装 `MTLDevice`、`CAMetalLayer`、`MTLCommandQueue`。
- Android: `RenderDeviceGLES` 封装 EGL/GLES3 context；`RenderDeviceVulkan` 封装 Vulkan device（未来）。

## 平台桥接 (PlatformBridge)

引擎核心不直接依赖平台 SDK。所有平台能力通过 `PlatformBridge` 注入：

```cpp
// src/platform/bridge/PlatformBridge.h
class PlatformBridge {
public:
    virtual ~PlatformBridge() = default;

    // 系统信号
    virtual void onMemoryPressure() = 0;
    virtual void onEnterBackground() = 0;
    virtual void onEnterForeground() = 0;

    // 网络（可取消）
    virtual std::unique_ptr<HttpRequest> createRequest(const std::string& url) = 0;

    // 文件系统
    virtual std::string cacheDirectory() const = 0;
    virtual std::string documentsDirectory() const = 0;

    // 图片解码
    virtual std::unique_ptr<DecodedImage> decodeImage(const uint8_t* data, size_t len) = 0;

    // 日志
    virtual void log(LogLevel, const std::string& tag, const std::string& msg) = 0;

    // 设备信息
    virtual DeviceInfo deviceInfo() const = 0;
};
```

## 运行时主循环

```text
平台输入事件 (iOS CADisplayLink / Android Choreographer)
  -> InputManager 归一化
  -> update Scene state
  -> build FrameState
  -> update TilePlans / Layers
  -> schedule requests and texture uploads
  -> build RenderCommands
  -> RenderDevice::submit(commands)
  -> update Diagnostics
```

网络请求和线程池解析只更新资源状态；当前帧最终渲染什么由 FrameState 和 RenderQueue 决定。

## Public API 示例

```cpp
class Engine {
public:
    // 构造时注入平台桥接
    Engine(std::unique_ptr<PlatformBridge> bridge);
    ~Engine();

    // 生命周期
    void onSurfaceCreated(void* nativeSurface);  // CAMetalLayer* / ANativeWindow*
    void onSurfaceChanged(int width, int height);
    void onSurfaceDestroyed();

    // 渲染
    void render();
    void requestRender();

    // 图层
    void addLayer(std::shared_ptr<Layer> layer);
    void removeLayer(const std::string& layerId);

    // 相机
    Camera& getCamera();
    void flyTo(const Cartographic& target, const FlyToOptions& options);

    // 拾取
    PickResult pick(float screenX, float screenY);

    // 时间
    void setTime(double julianDate);
    double getTime() const;

    // 诊断
    Diagnostics getDiagnostics() const;

private:
    class Impl;
    std::unique_ptr<Impl> d;
};
```

API 不应暴露内部 GPU 对象、tile cache map 或 provider 私有状态。

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

- `update(const FrameState&)`
- `buildRenderCommands(const FrameState&, RenderCommandList&)`
- `setVisible(bool)`
- `dispose()`
- `getAttribution() const`
- `getDiagnostics() const`

## Provider 生命周期

```text
created
  -> metadata-loading
  -> ready
  -> requesting
  -> failed
  -> disposed
```

Provider 不应知道具体 UI，不应长期持有 RenderDevice 资源。

## Renderer 职责

Renderer 负责：

- 通过 RenderDevice 管理 GPU 资源。
- 编译 shader/pipeline（MSL / GLSL ES / SPIR-V）。
- 执行 render pass。
- 管理 framebuffer。
- 提供 GPU 能力查询。
- 处理 context/device lost 和资源驱逐。
- 提供 GPU 资源统计。

Renderer 不负责：

- 选择业务图层。
- 计算 provider URL。
- 判断用户权限。
- 做 CRS 纠偏。

## 线程架构

引擎核心运行在主线程（输入、场景更新、渲染提交）。耗时操作在后台线程池执行：

- 网络请求：libcurl 或平台 HTTP（异步回调）。
- 瓦片解码：线程池（图片 → 原始像素缓冲区）。
- 几何解析：线程池（MVT、GeoJSON、glTF）。
- GPU 上传：主线程（RenderDevice 线程安全仅主线程）。

详见 `threading-architecture.md`。

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
- 线程池状态。

不要等引擎复杂后再补 debug。
