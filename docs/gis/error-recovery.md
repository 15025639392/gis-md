# 错误处理与恢复策略

地球引擎必须把失败当作常态：网络失败、token 失效、瓦片缺失、线程任务崩溃、GPU context/device lost、Metal resource eviction、内存压力、provider 越界都会发生。

## 错误分类

- `NetworkError`：断网、timeout、DNS、连接失败。
- `HttpError`：401、403、404、429、5xx。
- `DecodeError`：图片、MVT、terrain、3D Tiles 解析失败。
- `CrsError`：CRS 不支持、坐标转换失败。
- `TileSchemeError`：tile key 越界、matrix set 不匹配。
- `RenderError`：shader、buffer、texture、framebuffer。
- `ContextLostError`：Metal resource eviction / GL ES context lost / Vulkan device lost (VK_ERROR_DEVICE_LOST)。
- `MemoryPressureError`：缓存或 GPU 资源超预算。
- `PermissionError`：业务权限不足。
- `DataQualityError`：几何非法、nodata、字段缺失。

## 错误对象

```text
EngineError {
  code: string
  severity: "debug" | "warning" | "recoverable" | "fatal"
  source: "provider" | "tile" | "renderer" | "interaction" | "worker" | "core"
  layerId?: string
  providerId?: string
  tileKey?: TileKey
  message: string
  cause?: unknown
  recoverable: boolean
}
```

## 恢复策略

### 网络和 HTTP

- timeout：有限重试，指数退避。
- 404：标记 tile missing，不无限重试。
- 401/403：暂停 provider，提示认证/权限。
- 429：降并发并退避。
- 5xx：有限重试，保留 parent fallback。

### 解码失败

- 标记 tile failed。
- 记录 provider、tile key、content type。
- 不让坏瓦片进入 texture upload。
- debug 模式可显示错误占位。

### 线程任务失败

- 捕获线程池任务异常（通过 `std::future` 或回调中的 error code）。
- 标记任务 failed。
- 线程池保持运行，单个任务失败不影响其他任务。
- 过期任务结果丢弃。
- fatal 前保留主线程可取消状态。

### GPU Context Lost / Resource Eviction

- 停止提交 draw。
- 标记所有 GPU resource invalid。
- 保留 provider/raw cache（非 GPU 资源不丢失）。
- 接收到 context restored / 应用进入前台信号后，通过 RenderDevice 重建 shader、buffer、texture、framebuffer。
- Metal resource eviction：应用进入前台时检查 `MTLResource` 的有效性，按需重建。
- GL ES context lost：EGL context 重新创建后，全部 GL 资源需重建。
- Vulkan device lost：重建 VkDevice 及全部关联资源；通常由驱动崩溃或系统升级触发。
- 如果无法恢复，显示明确错误状态。

### 内存压力

- 降低缓存预算。
- 释放不可见 tile。
- 降低预取。
- 降低纹理分辨率或禁用重效果。
- 保留当前视域必要资源。

### 数据质量

- 非法 geometry 不应直接渲染。
- nodata 应有透明或占位策略。
- 字段缺失走样式 fallback。
- CRS 缺失必须要求声明或拒绝。

## 用户可见错误

用户需要看到：

- 图层加载失败。
- 权限/token 问题。
- 数据超限。
- 当前设备不支持 Metal / OpenGL ES 3.0 / Vulkan 能力。

用户不需要看到：

- 单个可 fallback 的 tile 404。
- debug 级别 shader 细节。

## 错误观测

Diagnostics 应记录：

- error count by code。
- failed tile count。
- retry count。
- provider paused state。
- context lost count。
- memory pressure events。

## 禁止做法

- 静默吞掉所有错误。
- 无限重试。
- 失败 tile 反复进入请求队列。
- context lost 后继续 draw。
- token 失效后继续打爆 provider。
- 数据非法时渲染随机结果。
