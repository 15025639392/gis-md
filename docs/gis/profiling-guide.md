# 性能剖析指南

本文件提供在 iOS 和 Android 设备上进行地球引擎性能分析的实操步骤。与 `performance-data-stability.md` 配合使用：后者定义性能指标和约束，本文件定义如何测量。

## 通用原则

1. **始终在真机上测试**，模拟器/仿真器的 GPU 行为和性能与真机差异巨大。
2. **固定测试场景**：每次使用相同的初始相机位置、相同的底图 provider、相同的设备状态（飞行模式、后台无应用）。
3. **预热后测量**：启动后等待 3-5 秒瓦片加载稳定，再开始记录。
4. **多次运行取中位数**：至少运行 5 次，取中位数，避免偶然波动。

## iOS: Xcode Instruments

### Metal System Trace

最全面的 GPU 性能分析工具。

**启动步骤：**

1. 在 Xcode 中打开项目，选择真机 target。
2. Product → Profile（⌘I）。
3. 选择 **Metal System Trace** 模板。
4. 点击录制按钮，在设备上操作 30 秒（旋转、缩放、切换图层）。
5. 停止录制。

**关键指标（在 Trace 中查看）：**

| 指标 | 位置 | 目标值 |
|------|------|--------|
| Frame time (ms) | GPU Timeline | < 16.7 ms (60 FPS) 或 < 33.3 ms (30 FPS) |
| GPU active time (%) | GPU Utilization | < 80%（留有 headroom 防掉帧） |
| CPU encode time (ms) | Command Buffer Timeline | < 5 ms（渲染命令编码时间） |
| Texture upload count | Resource Timeline | < 4/frame（见 basemap-tile-rendering.md） |
| Memory (MB) | Allocations | < 200 MB GPU + < 200 MB CPU |
| Shader compile time (ms) | Shader Compilation | 仅首次出现；后续帧应为 0 |

**常见问题定位：**

- **帧时间高但 GPU 空闲**：CPU 是瓶颈（瓦片解码、TilePlan 计算）→ 使用 Time Profiler 定位。
- **GPU 活跃但帧时间稳定**：draw call 或 shader 复杂度过高 → 降低三角形数、减少透明 pass。
- **Texture upload spikes**：一次上传过多 tile → 限制每帧上传数。

### Time Profiler

CPU 性能分析。

**启动步骤：**

1. Product → Profile → **Time Profiler**。
2. 录制 30 秒操作。
3. 双击 heaviest stack trace。

**关注的高耗时函数：**

- `TilePlanBuilder::buildDesiredTiles` — 应 < 2 ms
- `TileDecoder::decode` — 应在后台线程，主线程看不到
- `Renderer::buildRenderCommands` — 应 < 3 ms
- `RenderDevice::submit` — 应 < 10 ms（含 GPU 驱动开销）
- To 参考 `reference-architecture.md` 的模块职责

### Allocations

内存泄漏和内存压力检测。

**启动步骤：**

1. Product → Profile → **Allocations**。
2. 操作场景：旋转 2 分钟、缩放 30 次、切换图层 20 次。
3. 停止录制，查看 **Persistent Bytes** 是否持续增长。

**正常模式：**内存随 tile 加载上升，达到 cache 上限后稳定。图层切换后旧 tile 释放。
**异常模式：**内存单调递增不回落 → GPU 资源泄漏。

## Android: GPU Inspector + Perfetto

### Android GPU Inspector (AGI)

用于 Vulkan 和 GL ES 的帧级 GPU 分析。

**启动步骤：**

1. 安装 [Android GPU Inspector](https://gpuinspector.dev/)。
2. 连接设备（USB），启用 GPU 调试层。
3. 选择应用，开始 capture。
4. 操作 30 秒，停止。

**关键指标：**

| 指标 | 目标值 |
|------|--------|
| Frame duration | < 16.7 ms (60 FPS) 或 < 33.3 ms (30 FPS) |
| Draw calls/frame | < 200（Globe + 底图 + 适量矢量） |
| Primitives/frame | < 500K 三角形 |
| Texture memory | < 150 MB（中端设备） |
| Shader compile count | 0（首次启动后应缓存） |

### Perfetto

系统级性能追踪（CPU、GPU、内存、功耗）。

**启动步骤：**

1. 在设备上启用：Settings → Developer Options → System Tracing。
2. 或使用 [Perfetto UI](https://ui.perfetto.dev/) + adb 录制。
3. 录制 60 秒操作。
4. 在 `ui.perfetto.dev` 中打开 trace 文件。

**分析：**

- **CPU 调度**：主线程是否被抢占？渲染线程是否独占大核？
- **内存**：`mem.rss` 是否超过设备可用内存的 50%？
- **GPU 频率**：长时间高负载是否触发 thermal throttle？（GPU 频率陡降）
- **电量**：持续渲染时电流是否 > 500 mA？若 > 1000 mA 则需降低帧率。

## 标准测试场景

### 场景 1：冷启动

- 清除应用缓存。
- 启动应用到首屏地球完整显示。
- 测量：启动耗时、首帧时间、首屏全瓦片加载时间。

### 场景 2：快速操作

- 连续旋转 20 秒（手指不抬起）。
- 连续缩放 10 次（快速 pinch in/out）。
- 测量：帧率稳定性（掉帧次数）、tile 请求取消率。

### 场景 3：弱网

- 使用 Network Link Conditioner (iOS) 或 `adb shell cmd netemu` 模拟 3G 网络。
- 旋转/缩放 30 秒。
- 测量：白屏面积占比、请求队列长度、parent fallback 命中率。

### 场景 4：长时间运行

- 持续渲染 30 分钟（自动旋转地球）。
- 测量：内存趋势、GPU 资源数量趋势、是否有明显资源泄漏。

### 场景 5：应用生命周期

- 切换到 Home 屏 → 等待 10 秒 → 切回应用。
- 重复 10 次。
- 测量：GPU 资源重建时间、内存是否恢复基线。

## 性能目标（按设备等级）

| 指标 | 低端 (Mali G52, Adreno 610) | 中端 (Mali G78, Adreno 660) | 高端 (A17 Pro, Adreno 750) |
|------|------------------------------|------------------------------|-----------------------------|
| 目标帧率 | 30 FPS | 30 FPS (省电) / 60 FPS | 60 FPS |
| 帧时间预算 | 33 ms | 33 / 16.7 ms | 16.7 ms |
| 最大纹理数 | 100 | 200 | 400 |
| 最大可见 tile | 30 | 60 | 100 |
| GPU 内存预算 | 80 MB | 150 MB | 250 MB |
| 纹理上传/帧 | 2 | 4 | 6 |
| 线程池大小 | 1 | 2 | 3 |

设备等级在启动时通过 `PlatformBridge::deviceInfo()` 检测，引擎自动调整配置。
