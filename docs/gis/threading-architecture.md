# 线程架构

本文件定义地球引擎在 C++ 移动端环境中的线程模型。替代 Web Worker 范式。AI 在实现或修改异步任务时，必须先理解本文件中的线程边界、任务生命周期和平台约束。

## 设计目标

- 主线程零阻塞：渲染循环、输入处理、UI 更新必须在主线程以 60/30 FPS 执行。
- 耗时任务隔离：网络请求、图片解码、几何解析、3D Tiles content 处理在线程池执行。
- 任务可取消：相机移动后，旧帧的瓦片解码任务必须能取消或标记过期。
- GPU 上传仅主线程：Metal 和 OpenGL ES 的 GPU 资源创建和更新必须在主线程（或持有 GL context 的线程）执行。
- 移动端适应：线程池大小保守，避免与系统后台任务抢占 CPU 资源。

## 线程模型

```text
Main Thread (UI thread / render thread)
  ├── PlatformBridge::onEnterBackground/onEnterForeground
  ├── RenderDevice::submit(commands)
  ├── InputManager / GestureRecognizer
  ├── Scene update / Camera
  ├── TilePlan update
  ├── RenderCommand building
  └── GPU resource creation & upload

Background Thread Pool (2-3 threads)
  ├── HTTP request callbacks (libcurl async / platform HTTP)
  ├── Tile image decoding (CGImage / BitmapFactory / stb_image)
  ├── GeoJSON/MVT parsing
  ├── glTF / 3D Tiles content parsing
  ├── Terrain mesh generation
  ├── Geometry simplification
  └── Spatial index building

Platform I/O Threads (managed by platform)
  ├── iOS: NSURLSession delegate queue
  └── Android: OkHttp dispatcher / WorkManager for offline downloads
```

## 任务线程池

### 接口

```cpp
// src/threading/TaskThreadPool.h
class TaskThreadPool {
public:
    explicit TaskThreadPool(int numThreads = 2);
    ~TaskThreadPool();

    // 提交任务，返回 future。支持优先级。
    template<typename F>
    auto submit(TaskPriority priority, F&& task) -> std::future<decltype(task())>;

    // 取消所有与 token 关联的待处理任务
    void cancel(CancellationToken token);

    // 暂停/恢复（应用进入后台时暂停）
    void pause();
    void resume();

    // 统计
    struct Stats {
        int queuedTasks;
        int runningTasks;
        int completedTasks;
        int cancelledTasks;
    };
    Stats stats() const;

private:
    class Impl;
    std::unique_ptr<Impl> d;
};
```

### 任务优先级

```cpp
enum class TaskPriority {
    Critical,   // 当前视域瓦片解码（影响首屏）
    High,       // 当前视域附近瓦片
    Normal,     // 预取瓦片、非视域数据
    Low,        // 离线缓存写入、统计
};
```

### 取消令牌

```cpp
// src/threading/CancellationToken.h
class CancellationToken {
public:
    CancellationToken();
    ~CancellationToken();

    bool isCancelled() const;
    void cancel();

    // 从 token 创建子 token（父取消则子也取消）
    CancellationToken createChild();

private:
    class Impl;
    std::shared_ptr<Impl> d;
};
```

每个 TilePlan 帧生成一个新的 `CancellationToken`。当相机移动并生成新的 TilePlan 时，旧 token 被 cancel，所有关联的线程池任务在下次检查点后丢弃结果。

### 线程安全

- `TaskThreadPool::submit` 线程安全，可从任何线程调用。
- 任务回调在主线程执行（通过消息队列注入到主循环）。
- GPU 资源创建始终通过 `RenderDevice` 在主线程执行，线程池任务只输出 CPU 数据（像素缓冲区、几何数组、解析后的 JSON）。

## 平台集成

### iOS

- 主线程 = 主运行循环（CFRunLoop / CADisplayLink callback）。
- 线程池使用 `std::thread` + `std::mutex` + `std::condition_variable`，设置 QoS class 为 `QOS_CLASS_UTILITY`（通过 `pthread_set_qos_class_self_np`）。
- 应用进入后台时调用 `TaskThreadPool::pause()`，进入前台时调用 `resume()`。
- NSURLSession 的回调队列应与线程池分开，避免互相阻塞。

### Android

- 主线程 = UI thread（Looper.getMainLooper() / Choreographer.FrameCallback）。
- 线程池使用 `std::thread`，优先级通过 `setpriority(PRIO_PROCESS, ...)` 降低。
- 应用 onPause 时调用 `pause()`，onResume 时调用 `resume()`。
- 避免在后台线程中直接调用 JNI（需要 `JNIEnv*` 线程绑定）。

## 数据流：瓦片加载示例

```text
FrameState (Main Thread)
  -> TilePlanBuilder::buildDesiredTiles()
  -> 检查 TileCache
  -> 缺失瓦片 → TileRequestScheduler

TileRequestScheduler (Main Thread 发起)
  -> 创建 CancellationToken (frameId)
  -> PlatformBridge::createRequest(url)
  -> HTTP 响应到达 (Platform I/O Thread)
     -> RawCache 存储原始字节
     -> 提交解码任务到 TaskThreadPool (TaskPriority::Critical)

TaskThreadPool (Background Thread)
  -> TileDecoder::decode(rawBytes) → 像素缓冲区
  -> 检查 CancellationToken::isCancelled()
  -> 如果未取消：结果回传主线程

Main Thread
  -> 接收解码结果
  -> RenderDevice::createTexture(像素缓冲区)
  -> TextureCache 存储
  -> 标记 tile 为 texture-ready
  -> 下一帧 render queue 使用该纹理
```

## 应用生命周期

应用进入后台时：

1. `TaskThreadPool::pause()` — 暂停新任务调度，正在运行的任务可完成。
2. 取消所有网络请求（平台可能强制超时）。
3. 保留 CPU 缓存（raw/decoded），释放部分 GPU 资源（Metal resource eviction 会自动驱逐）。

应用回到前台时：

1. `TaskThreadPool::resume()`。
2. 检查 GPU 资源有效性，重建被驱逐的资源。
3. 重新发起因超时取消的网络请求（基于当前 frame state）。

## 故障处理

- 线程池任务抛出异常 → 通过 `std::future` 传播，主线程记录错误并标记 tile failed。
- 线程池线程崩溃 → 捕获 `std::terminate`，重启线程，通知 diagnostics。
- 死锁检测 → 线程池队列超过 N 秒无消费时输出警告日志。

## 验收清单

- 线程池任务在相机快速移动后能被取消（旧任务结果不覆盖当前帧）。
- 应用后台/前台切换后，线程池正确暂停/恢复。
- 线程池统计信息可通过 diagnostics 查看。
- 线程池不创建超过配置数量的工作线程。
- 长时间运行（30 分钟以上）无内存泄漏或线程泄漏。
