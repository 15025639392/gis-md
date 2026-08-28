# 继续任务提示词

请在新窗口中使用以下提示词继续当前任务：

---

## 提示词

我在 `/Users/ldy/Desktop/work/gis-md` 项目的 `codex/surface-instancing-gpu-batch` 分支上优化 Android MinimalGlobe Demo。当前连接设备 `7e045e39`。

### 已完成的修复（全部 ✅）

**1. Shader 纹理采样器超限**（黑屏）
- `Renderer.cpp`: 注释掉 10 个 PBR 扩展纹理采样器

**2. 地形 glTF 瓦片不渲染**
- `MinimalGlobeDemoConfig.h`: 服务器地址更新为 `192.168.1.6:8090`
- `QuantizedMeshParser.cpp:265`: 修复 4 字节对齐填充

**3. 性能优化（FPS 从 ~5 提升到 ~25）**

| 修改 | 文件 | 内容 |
|---|---|---|
| 上传限制 | `Tileset.cpp:25` | `kSmoothedMainThreadUploadLimit` 1→4 |
| 预算放宽 | `TileFrameResourceBudgetPlanner.h:84-95` | 平滑模式限制 1→4, 栅格 4→8 |
| 移除 glFlush | `RenderDeviceGLES.cpp:952` | 删除 `glFlush()` |
| 地形轻量顶点 | `GltfRenderGeometryBuilder.h/cpp` | 新增 `TerrainGpuVertex`(32字节) |
| 地形轻量顶点 | `GltfRenderResourcePreparer.cpp:488` | 地形使用 32 字节格式 |
| 地形轻量顶点 | `GltfDrawCommandBuilder.cpp:80` | 设置 `vertexStride=32` + 地形 shader |
| 地形轻量顶点 | `Renderer.cpp` | 新增 `kTerrainVertexGLSL/kTerrainFragmentGLSL` |
| 地形轻量顶点 | `TileRenderContentState.h` | 新增 `useTerrainVertexFormat` 标志 |
| 禁用性能杀手 | `LoadedTerrainHeightSampler.cpp:212` | 禁用 O(N×M) 地形高度采样 |

**4. 调试日志**（临时，待清理）
- `TilePendingLoadProcessor.h`: 添加 `processPendingLoads` 计数日志
- `TileLoadLifecycle.cpp`: 添加 `containsWorkForCacheKey` 状态日志

### 当前状态

**性能数据**（相机静止时）：
```
FPS: ~25 (目标 60)
帧时间: ~40ms
├── update: 4-8ms (正常)
│   ├── camera: 0ms ✅ (禁用高度采样后)
│   ├── selector: 0.6-1.2ms ✅ (Strict reuse)
│   ├── prefetch: 3-10ms ❌ (仍然慢)
│   └── request: 0.26ms ✅
├── render: 10-16ms
│   ├── layers: 8-12ms
│   └── submit: 0.5-1.5ms ✅
瓦片数: 6-9 个
渲染命令: 8-11 个
```

**性能数据**（相机移动时）：
```
FPS: ~15
帧时间: ~65ms
├── update: 35-45ms ❌
│   ├── camera: 0ms ✅
│   ├── selector: 11-14ms ❌ (遍历 19 个瓦片)
│   ├── prefetch: 6-7ms ❌
│   └── terrainUpload: 35-40ms ❌ (164 个加载请求)
├── render: 25-30ms ❌ (69-89 个渲染命令)
瓦片数: 35-40 个
渲染命令: 69-89 个
```

### 待解决的 3 个核心问题

**问题 1：高德卫星影像不能加载到 zoom 18（最关键）**
- 现象：相机拉到地面时，卫星影像仍是低分辨率（高空瓦片）
- 地形数据最高 zoom 12（`layer.json` 确认）
- 栅格叠加 `overlayMaximumZoom=0` → `getMaximumLevel()` 返回 scheme 的 maxZoom=25
- `computeDesiredScreenPixels` 计算表明 zoom 应该能到 18+
- 调试日志显示 `requestState=0, pendingLoads=0`（lifecycle 为空）
- `load=31` 是 loadQueue 大小，每帧由 prefetch 添加，被 `requestMissingTiles` 正确跳过

**已追查并修复的 3 个根因：**
1. **MSE 双重计算**（已修复）：`TileUpdateSelectionWorkRunner` 传递瓦片集 MSE（16.0）给 `computeDesiredScreenPixels`，但 `chooseQuadtreeSourceZoom` 再次除以 provider MSE（2.0），导致 `targetScreenPixels` 被除以 8 倍
   - 修复：`TileRasterOverlayPrefetcher::prefetch()` 改用 `activeProvider->getMaximumScreenSpaceError()`（2.0）
2. **地形几何误差过大**（已修复）：zoom-12 地形瓦片 `geometricError ≈ 150m`，导致 `targetScreenPixels ≈ 453`，仅计算到 zoom 13
   - 修复：`RasterOverlayScreenSpaceMetrics.cpp` 添加 `kMaximumEffectiveGeometricError = 5.0` 上限
3. **WebMercator 坐标系不匹配**（已修复）：`schemeDimensionsForRectangle` 对 WebMercator 使用地理弧度宽度，但 `rootTileWidth` 使用投影弧度（2π），导致 zoom 计算偏差
   - 修复：WebMercator 方案改用投影米制宽度/高度

**剩余限制：地理瓦片网格**
- 修复后 zoom 从 13 提升到 **15**（2→24 个源瓦片，分辨率提升 4 倍）
- zoom 18 在纬度 30° 不可行：0.044° 经度 = 32 个瓦片 = 8192 像素 > 2048 纹理限制
- `computeCoverage` 使用地理坐标计算瓦片数，在高纬度地区瓦片数被放大（cos(30°) ≈ 0.87）
- 若要达到 zoom 18，需要将 `maximumTextureSize` 提升到 8192（需验证 GPU 支持）

**问题 2：瓦片选择器在相机移动时 11-14ms（应该是 <1ms）**
- 根因：`visitTile` 中调用 `TileSelectionRasterOverlayPreparer::prepare()`
- 每个瓦片 0.58ms 来自 `TileRasterOverlayPrefetcher::prefetch()`
- prefetch 内部调用 `overlay->ensureTileProvider()` 并推进 Direct raster attachment 状态
- **需要优化**：将栅格叠加预取从选择遍历中移出

**问题 3：prefetch 在 Strict reuse 时仍然 3-10ms**
- 根因：`TileUpdateSelectionWorkRunner::run()` 中即使 `reusedSelection=true` 也执行 prefetch
- 代码位置：`TileUpdateSelectionWorkRunner.h:86-106`
- **需要优化**：Strict reuse 时跳过 prefetch

### 关键代码路径

```
帧循环:
Engine::render(deltaSeconds)
├── beginFrame()                    // RenderDeviceGLES
├── scene_->update(deltaSeconds)    // SceneFrameUpdateCoordinator
│   ├── cameraController->update()  // 0ms (高度采样已禁用)
│   ├── tilesets->update()          // TilesetUpdateFrameFacade
│   │   └── TilesetUpdateFrameRuntime::run()
│   │       ├── reuseMode 判断       // TileSelectionReusePolicy
│   │       ├── 选择/刷新            // TileUpdateSelectionWorkRunner
│   │       │   ├── selectTiles()    // 仅非 reuse 时
│   │       │   ├── prefetchSelection()  // 每帧都执行 ← 问题 3
│   │       │   └── requestMissingTiles() // TileLoadScheduler
│   │       └── processPendingLoads()    // TilePendingLoadProcessor
│   └── scene_->render()           // SceneRenderPipeline
│       ├── buildStableLayerCommands()  // TilesetRenderFrameExecutor
│       │   └── TileRenderEntryCommandBuilder::build()
│       ├── applyMvpUniforms()
│       └── renderer->submit()     // RenderDeviceGLES::submit
└── endFrame()                     // 已移除 glFlush
```

### 地形数据

```
服务器: http://192.168.1.6:8090
数据: /Users/ldy/Desktop/work/dem_test/data/tiles/fabdem-quantized-mesh-chongqing
Zoom 范围: 0-12 (layer.json)
Bounds: [105.17, 28.1, 110.2, 32.25] (重庆)
Tile scheme: Geographic-TMS
高德卫星: zoom 0-18, webst0{s}.is.autonavi.com
高德路网: zoom 0-18, webst0{s}.is.autonavi.com
```

### 快速开始

```bash
# 启动地形服务器
cd /Users/ldy/Desktop/work/dem_test/data/tiles/fabdem-quantized-mesh-chongqing
python3 -m http.server 8090

# 构建并安装
cd /Users/ldy/Desktop/work/gis-md/scaffold/examples/android
./gradlew assembleDebug && adb install -r app/build/outputs/apk/debug/app-debug.apk
adb shell am start -n com.earthengine.minimalglobe/.MainActivity

# 查看性能日志
adb logcat -s MinimalGlobe GLES EarthPerf Lifecycle PendingLoad | grep -E "submit|update|selector|prefetch|Lifecycle|PendingLoad"

# 触发相机移动测试
adb shell input swipe 500 1000 500 500 200
```

### 下一步任务（按优先级）

1. ~~**追查高德卫星影像 zoom 问题**~~ ✅ 已修复（zoom 13→15），剩余限制是地理瓦片网格
2. ~~**优化 Strict reuse 时的 prefetch**~~ ✅ 已修复（`TileUpdateSelectionWorkRunner.h` reuse 时跳过 prefetch+request，3-10ms→0ms）
3. ~~**清理调试日志**~~ ✅ 已清理（`TilePendingLoadProcessor.h`、`TileLoadLifecycle.cpp`）
4. **验证 zoom 15 影像视觉效果**：安装最新 APK，确认卫星影像在地面视角是否清晰
5. ~~**优化 rasterUpload 瓶颈**~~ ✅ 已修复（架构改造完成）
   - 根因：`GltfRenderResourcePreparer::prepare()` 在主线程同步执行 CPU+GPU 工作
   - **改造方案：CPU/GPU 三阶段流水线**
     - 新增 `GpuReadyData.h` — GPU 就绪数据结构
     - 新增 `GpuUploadQueue.h` — 线程安全队列
     - 改造 `GltfRenderResourcePreparer` — 拆分 `prepareCpuWork()` + `uploadToGpu()`
     - 改造 `TilesetContentLifecycleCoordinator` — 地形内容通过 `AsyncSystem::run()` 在 Worker Thread 执行 CPU 工作
     - 改造 `TileContentRuntime.h/cpp` — 传递 `GpuUploadQueue`
     - 改造 `TileContentLifecycleManager.h` — 传递 `GpuUploadQueue`
     - 改造 `Tileset.h/cpp` — 持有 `GpuUploadQueue` 实例
   - **效果**：`terrainUpload` 从 40-80ms 降到 0.02ms（提升 2000-4000 倍）
   - **线程安全**：修复 use-after-free bug — Worker Thread 深拷贝 `GltfPrimitive` 数据而非捕获裸指针

6. **优化瓦片选择器性能**（相机移动时 21ms）：
   - 根因：`visitTile` 中 `TileSelectionRasterOverlayPreparer::prepare()` 对每个瓦片调用 `TileRasterOverlayPrefetcher::prefetch()`
   - 每瓦片 0.58ms，36 瓦片 = 21ms
   - 注意：不能完全移除 `prepare()`，否则瓦片永远不会变为 ready（已验证）
   - 方案：优化 `canSkipReadyOverlayPrefetch` 快速路径，减少不必要的 prefetch 调用

### 最优架构设计：三阶段流水线

**问题根因**：OpenGL ES 要求 GL 调用在 GL context 线程，但代码把 CPU 工作（buildVertices）和 GPU 工作（createBuffer）捆绑在一起，导致 CPU 工作也阻塞主线程。

**架构**：
```
阶段 1: 网络加载（已有 ✅）
  线程: Worker Thread (curl)
  耗时: 10-100ms（不阻塞主线程）

阶段 2: CPU 准备（需要改造）
  线程: AsyncSystem::pool()（已有线程池）
  工作: buildVertices + decodeTexture + computeProjection
  耗时: 5-20ms（不阻塞主线程）
  输出: GpuReadyData（顶点字节 + 索引字节 + 纹理像素）

阶段 3: GPU 上传（需要改造）
  线程: Main Thread（GL context）
  工作: glBufferData + glTexImage2D
  耗时: 3-8ms（阻塞主线程，但很短）
  输入: GpuReadyData
```

**需要修改的文件**：
1. 新增: `GpuReadyData.h`（GPU 就绪数据结构）
2. 修改: `GltfRenderResourcePreparer.cpp`（拆分 prepareCpuWork + uploadToGpu）
3. 修改: `TilePendingLoadProcessor.h`（两阶段处理）
4. 修改: `TilesetContentLifecycleCoordinator.h`（异步 CPU 准备）
5. 修改: `TilePendingLoadCommitCoordinator.h`（GPU 上传阶段）

---
