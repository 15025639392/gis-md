# Android MinimalGlobe 性能问题深度分析

**日期**：2026-06-28
**分支**：codex/surface-instancing-gpu-batch
**状态**：地形轻量顶点路径 **绘制侧已接通**（2026-07-01 晚）—— 生产/上传/绘制三侧齐全

---

## 更新 2（2026-07-01 晚）：绘制侧已接通 ✅

下面"更新 1"里标 ❌ 的两项(绘制侧 + 地形 shader)**已实现**：新增 `kTerrainVertexGLSL/FragmentGLSL` + `kTerrainVertexMSL/FragmentMSL`(轻量地形 shader,= glTF shader 去掉 PBR 扩展)、`Renderer::terrainShader()` 定义 + `makeTerrainPrimitiveCommand`(stride 32,kind=GltfPrimitive)、`GltfDrawCommandBuilder` 按 `useTerrainVertexFormat` 分支。Metal 复用 `PipelineLayout::Surface`(已是 stride-32)+ terrain 专用 uniform 绑定表(buffer 0-23,≤30 未超限)。**验证**:native 138/138;macOS Metal 金丝雀确认 terrain shader 编译干净(不在任何 Metal 编译错误里);MSL↔绑定表 index 0-23 精确一一对齐。**未验证(需环境)**:实际像素渲染 —— QM 地形服务器 `192.168.1.6:8090` 在此环境不可达,需在你本机(macOS Metal / Android GLES)对可达服务器跑 demo 目视确认。(注:83-uniform glTF shader 在 Metal 仍编不过,是**独立** bug,只影响 glTF 模型,与地形无关,已单列。)

---

## 更新 1（2026-07-01，对照当前工作树代码）

> ⚠️ 本节的 ❌ 结论已被上方"更新 2"取代 —— 绘制侧当时未接通,现已接通。以下保留为历史。

下面的"已完成 ✅"标注经回源核对后修正 —— 地形轻量顶点格式（P0 第 3 项）只完成了 **生产 + 上传** 侧，**绘制侧尚未接通**：

| 子项 | 文档原标注 | 实际 | 证据 |
|---|---|---|---|
| `TerrainGpuVertex`(32B) + `buildTerrainVertices()` | ✅ | ✅ 已完成 | `GltfRenderGeometryBuilder.h:31-39`（`static_assert sizeof==32`）、`.h:75`/`.cpp:225` |
| `GltfRenderResourcePreparer` 用轻量格式 | ✅ | ✅ 已完成 | `useTerrainVertexFormat` 设置于 `.cpp`（含 async `prepareCpuWork`/`uploadToGpu`） |
| `TileRenderContentState.useTerrainVertexFormat` | ✅ | ✅ 已完成 | `TileRenderContentState.h` |
| `kSmoothedMainThreadUploadLimit` 1→4 | ✅ | ✅ 已完成 | `Tileset.cpp:25` |
| 移除 `glFlush()` | ✅ | ✅ 已完成 | `RenderDeviceGLES.cpp:952-956` |
| **`GltfDrawCommandBuilder.cpp` 设 32B stride + 地形 shader** | ✅ | ❌ **未完成** | 仍无条件走 `makeGltfPrimitiveCommand`（stride 120），不引用 `useTerrainVertexFormat` |
| **`Renderer.cpp` 新增 `kTerrainVertexGLSL/kTerrainFragmentGLSL` + `terrainShader` 成员/初始化** | ✅ | ❌ **未完成** | `Renderer.cpp` 中 grep 无任何命中；`Renderer::terrainShader()` 在 `Renderer.h:66` 仅有声明、无定义 |

**后果**：走 async 路径的地形（当前仅 `QuantizedMeshContentLoader` 预打包 `terrainGpuVertexBytes`）会以 32 字节上传、却以 120 字节 stride + glTF shader 绘制 → 顶点错位/越界。该分支地形轻量顶点路径尚未端到端可渲染。下文各处 "已完成 ✅" 涉及绘制侧/地形 shader 的部分，以本表为准。

---

## 问题现象

- FPS 低（~35 FPS，目标 60 FPS）
- 卡顿、发烫
- 渲染效果不符合预期（截图显示地形纹理异常）

---

## 核心问题：为什么测试全过但实际效果严重不符？

### 答案：测试的 Mock RenderDevice 跳过了所有 GPU 工作

```cpp
// test_tile_surface.cpp 中的 RecordingRenderDevice
class RecordingRenderDevice final : public RenderDevice {
    std::unique_ptr<ShaderProgram> createShader(const ShaderDesc&) override {
        return nullptr;  // ← 跳过 shader 编译
    }
    std::unique_ptr<Texture> createTexture(const TextureDesc&) override {
        return std::make_unique<DummyTexture>(...);  // ← 假纹理，不上传 GPU
    }
    std::unique_ptr<Buffer> createBuffer(const BufferDesc&) override {
        return std::make_unique<DummyBuffer>(...);   // ← 假缓冲区，不分配显存
    }
};
```

**测试验证的是**：CPU 侧的数据流、瓦片选择逻辑、渲染命令构建
**测试无法验证**：Shader 编译、纹理上传、GPU 绘制、性能、热管理

---

## 根因分析

### 根因 1：主线程上传瓶颈（最严重）

**文件**：`scaffold/src/earth_engine/tiling/Tileset.cpp:25`

```cpp
constexpr int kSmoothedMainThreadUploadLimit = 1;
```

**问题**：
- 资源平滑模式下，每帧只允许 **1 个纹理上传**
- 60 FPS = 每秒最多 60 个纹理上传
- 地形瓦片 + 影像叠加 = 需要数百个纹理
- 结果：瓦片在 "loading" 状态停留数十帧，用户看到未纹理化的地形

**预算规划**（`TileFrameResourceBudgetPlanner.h:86-100`）：
```cpp
config.maxMainThreadFinalizesPerFrame =
    input.resourceSmoothingActive ? 1u  // ← 平滑模式下硬限 1
    : input.maximumSimultaneousTileLoads;
config.maxRasterUploadsPerFrame =
    input.resourceSmoothingActive
        ? std::min<uint32_t>(4u, input.maximumSimultaneousTileLoads)  // ← 平滑模式下限 4
        : input.maximumSimultaneousTileLoads;
```

### 根因 2：glTF 地形顶点格式过重

**文件**：`scaffold/src/earth_engine/renderer/Renderer.cpp:2275`

```cpp
cmd.vertexStride = 120;  // POSITION/NORMAL + TEXCOORD_0..7 + COLOR_0 + TANGENT
```

**问题**：
- 每个顶点 120 字节（12+12+32+16+16+32 = 120）
- 64×64 地形网格 = 4,096 顶点 × 120 字节 = **491 KB / 瓦片**
- ~20 个可见瓦片 = **9.8 MB / 帧** 顶点数据
- 移动端内存带宽严重受限

**实际需求**：
- 地形只需要：POSITION(12) + NORMAL(12) + TEXCOORD_0(8) = **32 字节**
- 浪费了 88 字节/顶点（73% 空间浪费）

### 根因 3：glTF Shader 复杂度过高

**文件**：`scaffold/src/earth_engine/renderer/Renderer.cpp:229-843`

```glsl
// glTF fragment shader 有 550+ 行 PBR 计算
// 包含：法线贴图、金属粗糙度、遮挡、自发光、各向异性、清漆、光泽
// 即使注释掉 10 个扩展纹理，shader 仍然包含所有计算分支
```

**问题**：
- 移动端 GPU（Adreno/Mali）对复杂 shader 极其敏感
- 即使纹理被注释，shader 仍包含所有 uniform 声明和计算路径
- 每个地形瓦片都使用这个重量级 shader
- 导致 GPU 发热 → 热降频 → FPS 下降

### 根因 4：缺少 VAO（Vertex Array Object）

**文件**：`scaffold/src/earth_engine/platform/android/RenderDeviceGLES.cpp:468-672`

```cpp
// 每帧每命令都重新绑定所有顶点属性
for (const auto& cmd : commands) {
    // 15 个顶点属性逐个绑定
    setAttribEnabled(0, attrib0Enabled, true);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, vertexStride, ...);
    glVertexAttribDivisor(0, 0);
    setAttribEnabled(1, attrib1Enabled, true);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, vertexStride, ...);
    // ... 重复 15 次
}
```

**问题**：
- OpenGL ES 3.0 支持 VAO，但代码没有使用
- 每帧 20+ 个瓦片 × 15 个属性 = **300+ 次 glVertexAttribPointer 调用**
- VAO 可以缓存这些状态，避免重复设置

### 根因 5：glFlush() 强制同步

**文件**：`scaffold/src/earth_engine/platform/android/RenderDeviceGLES.cpp:956`

```cpp
void RenderDeviceGLES::endFrame() {
    glFlush();  // ← 强制 GPU 命令队列刷新，导致 CPU 等待 GPU
}
```

**问题**：
- `glFlush()` 会阻塞 CPU 直到 GPU 完成所有命令
- 应该使用 `glFenceSync()` 或直接依赖 `eglSwapBuffers()` 的隐式同步
- 这个调用导致 CPU 和 GPU 无法并行工作

### 根因 6：Per-Frame Uniform 重算

**文件**：`scaffold/src/earth_engine/scene/SceneRenderCommandUniformUpdater.cpp`

每帧对每个瓦片重新计算 MVP 矩阵：
```cpp
// 伪代码
for (auto& command : commands) {
    glm::mat4 mvp = proj * view * model;  // 每瓦片重新计算
    command.uniforms["u_modelViewProjection"] = ...;
}
```

**问题**：
- ~20 个瓦片 × 16 个 float × 4 字节 = 1.28 KB/帧（数据量不大）
- 但每个瓦片都需要 `uniformLocation()` 查询（字符串哈希）
- `std::unordered_map<std::string, std::vector<float>>` 有堆分配开销

### 根因 7：网络请求阻塞主线程

**文件**：`scaffold/src/earth_engine/tiling/TilesetUpdateFrameFacade.cpp:26-30`

```cpp
perf::logTimingAtLeast(frameState.frameId,
                       "Tileset.update",
                       perf::nowMs() - updateStartMs,
                       10.0,  // ← 只记录 >10ms 的调用
                       updateDetail.data());
```

**问题**：
- CONTINUE_PROMPT 中提到 "prefetch=17ms"
- 这意味着每帧有 17ms 花在瓦片选择和预取上
- 如果在主线程执行网络请求，会直接阻塞渲染

---

## 渲染流程分析

### 当前帧时序（估算）

```
帧开始
├── beginFrame()           ~0.5ms
├── update()               ~17ms (prefetch 瓶颈)
│   ├── camera.update()    ~0.1ms
│   ├── environment.update() ~0.2ms
│   └── tileset.update()   ~17ms  ← 瓶颈
│       ├── 瓦片选择        ~5ms
│       ├── 预取请求        ~10ms
│       └── 纹理上传        ~2ms (1 个纹理限制)
├── render()               ~3ms
│   ├── buildCommands()    ~1ms
│   └── submit()           ~2ms (20+ draw calls)
├── endFrame()             ~2ms (glFlush 同步)
└── eglSwapBuffers()       ~1ms
───────────────────────────
总计                       ~26ms → ~38 FPS
```

### 理想帧时序（优化后）

```
帧开始
├── beginFrame()           ~0.3ms
├── update()               ~3ms
│   ├── camera.update()    ~0.1ms
│   ├── environment.update() ~0.2ms
│   └── tileset.update()   ~2.7ms (预算限制 + 异步上传)
├── render()               ~2ms
│   ├── buildCommands()    ~0.5ms (VAO 缓存)
│   └── submit()           ~1.5ms (批量绘制)
├── endFrame()             ~0.2ms (移除 glFlush)
└── eglSwapBuffers()       ~1ms
───────────────────────────
总计                       ~8ms → 120+ FPS
```

---

## 修复方案（按优先级）

### P0：立即修复（解决卡顿和 FPS）—— 已完成 ✅

#### 1. 增加主线程上传限制

**文件**：`scaffold/src/earth_engine/tiling/Tileset.cpp`

```cpp
// 改前
constexpr int kSmoothedMainThreadUploadLimit = 1;

// 改后
constexpr int kSmoothedMainThreadUploadLimit = 4;  // 允许 4 个并发上传
```

**文件**：`scaffold/src/earth_engine/tiling/TileFrameResourceBudgetPlanner.h`

```cpp
// 改前
config.maxMainThreadFinalizesPerFrame =
    input.resourceSmoothingActive ? 1u : ...;

// 改后
config.maxMainThreadFinalizesPerFrame =
    input.resourceSmoothingActive ? 4u : ...;  // 平滑模式下允许 4 个

config.maxRasterUploadsPerFrame =
    input.resourceSmoothingActive
        ? std::min<uint32_t>(8u, input.maximumSimultaneousTileLoads)  // 允许 8 个
        : input.maximumSimultaneousTileLoads;
```

#### 2. 移除 glFlush()

**文件**：`scaffold/src/earth_engine/platform/android/RenderDeviceGLES.cpp`

```cpp
// 改前
void RenderDeviceGLES::endFrame() {
    glFlush();
}

// 改后
void RenderDeviceGLES::endFrame() {
    // eglSwapBuffers() 会隐式等待 GPU 完成
    // 不需要显式 glFlush()
}
```

#### 3. 地形瓦片使用轻量顶点格式（32 字节）—— 部分完成 ⚠️（绘制侧未接通，见顶部核对更新）

**新增文件/修改**：

1. **`GltfRenderGeometryBuilder.h`** — 添加 `TerrainGpuVertex` 结构体（32 字节）✅
2. **`GltfRenderGeometryBuilder.cpp`** — 添加 `buildTerrainVertices()` 函数 ✅
3. **`GltfRenderResourcePreparer.cpp`** — 检测地形内容并使用轻量顶点格式 ✅
4. **`GltfDrawCommandBuilder.cpp`** — 设置 32 字节 stride 和地形 shader ❌ **未完成**（仍走 stride-120 `makeGltfPrimitiveCommand`）
5. **`TileRenderContentState.h`** — 添加 `useTerrainVertexFormat` 标志 ✅
6. **`Renderer.cpp`** — 添加地形专用轻量 shader（kTerrainVertexGLSL/kTerrainFragmentGLSL）❌ **未完成**（`Renderer.cpp` 无此 shader；`terrainShader()` 仅声明未定义）

**顶点格式对比**：

| 格式 | 字节 | 包含内容 |
|---|---|---|
| `GltfGpuVertex` | 120 | pos + nrm + 8×texcoord + color + tangent |
| `TerrainGpuVertex` | 32 | pos + nrm + texcoord0 |

**空间节省**：
- 每个地形瓦片：4,096 顶点 × (120-32) 字节 = **360 KB 节省**
- 20 个瓦片：**7.2 MB/帯宽节省**

**地形 shader 特点**：
- 使用 32 字节顶点布局
- 支持 4 个栅格叠加（u_mappedRasterTexture0..3）
- 支持水体遮罩
- 简化 PBR 计算（无扩展纹理）
- 预计 GPU 计算量减少 60%+

### P1：短期优化（解决发烫）

#### 4. 使用 VAO 缓存顶点状态

**文件**：`scaffold/src/earth_engine/platform/android/RenderDeviceGLES.cpp`

```cpp
// 添加 VAO 缓存
std::unordered_map<uint64_t, GLuint> vaoCache_;

// 在 submit() 中
for (const auto& cmd : commands) {
    uint64_t vaoKey = hash(cmd.vertexBuffer, cmd.indexBuffer, cmd.vertexStride);
    if (vaoCache_.find(vaoKey) == vaoCache_.end()) {
        GLuint vao;
        glGenVertexArrays(1, &vao);
        glBindVertexArray(vao);
        // 设置顶点属性...
        vaoCache_[vaoKey] = vao;
    }
    glBindVertexArray(vaoCache_[vaoKey]);
    glDrawElements(...);
}
```

#### 5. 为地形创建专用轻量 Shader

**文件**：`scaffold/src/earth_engine/renderer/Renderer.cpp`

```glsl
// 地形专用 shader（简化版 SurfaceTile）
#version 300 es
precision mediump float;

in vec2 v_texcoord;
in vec3 v_normal;
uniform sampler2D u_tileTexture;
uniform sampler2D u_overlayTexture0;
uniform vec3 u_lightDir;
out vec4 fragColor;

void main() {
    vec4 color = texture(u_tileTexture, v_texcoord);
    vec4 overlay = texture(u_overlayTexture0, v_overlayUv);
    color.rgb = mix(color.rgb, overlay.rgb, overlay.a);

    float shade = mix(0.72, 1.0, max(dot(normalize(v_normal), normalize(u_lightDir)), 0.0));
    color.rgb *= shade;
    fragColor = color;
}
```

### P2：中期优化（提升体验）

#### 6. 异步纹理上传

使用 PBO（Pixel Buffer Object）实现异步上传：
```cpp
// 创建 PBO
GLuint pbo;
glGenBuffers(1, &pbo);
glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo);
glBufferData(GL_PIXEL_UNPACK_BUFFER, size, nullptr, GL_STREAM_DRAW);

// 异步映射和上传
void* ptr = glMapBufferRange(GL_PIXEL_UNPACK_BUFFER, 0, size, GL_MAP_WRITE_BIT);
memcpy(ptr, data, size);
glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
glTexImage2D(..., GL_PIXEL_UNPACK_BUFFER, 0);  // 从 PBO 异步上传
```

#### 7. 减少 Uniform 查询开销

缓存 uniform location，避免每帧字符串哈希：
```cpp
// 在 shader 创建时缓存所有 location
std::unordered_map<std::string, GLint> uniformCache_;

// 在 submit() 中直接使用缓存
GLint mvpLoc = uniformCache_["u_modelViewProjection"];
glUniformMatrix4fv(mvpLoc, 1, GL_FALSE, mvp);
```

#### 8. 瓦片网格 LOD

根据相机距离使用不同精度的网格：
```cpp
// 远距离瓦片：16×16 = 256 顶点
// 中距离瓦片：32×32 = 1,024 顶点
// 近距离瓦片：64×64 = 4,096 顶点
int gridSize = selectGridSize(cameraDistance);
```

---

## 验证方法

### 1. GPU Profiling

使用 Android GPU Inspector（AGI）：
```bash
# 安装 AGI
# 连接设备
# 录制帧
# 分析：
# - GPU 时间分布
# - 纹理上传耗时
# - Draw call 数量
# - Shader 执行时间
```

### 2. 添加性能日志

在关键路径添加计时：
```cpp
auto start = std::chrono::high_resolution_clock::now();
// ... 执行操作 ...
auto end = std::chrono::high_resolution_clock::now();
double ms = std::chrono::duration<double, std::milli>(end - start).count();
LOGI("操作耗时: %.2f ms", ms);
```

### 3. 验证修复效果

```bash
# 构建并安装
cd scaffold/examples/android
./gradlew assembleDebug && adb install -r app/build/outputs/apk/debug/app-debug.apk

# 启动 demo
adb shell am start -n com.earthengine.minimalglobe/.MainActivity

# 查看 FPS
adb logcat -s MinimalGlobe | grep "FPS"

# 查看渲染统计
adb logcat -s GLES | grep "submit"

# 目标：
# - FPS ≥ 55
# - draw calls ≤ 30
# - 纹理上传 ≤ 8/帧
# - CPU 时间 ≤ 10ms/帧
```

---

## 修改总结

### P0 修复状态（4 项完成 / 2 项未完成）

| 修复项 | 文件 | 修改内容 | 预期效果 | 状态 |
|---|---|---|---|---|
| 增加上传限制 | `Tileset.cpp` | `kSmoothedMainThreadUploadLimit`: 1→4 | 瓦片加载快 4 倍 | ✅ |
| 放宽预算限制 | `TileFrameResourceBudgetPlanner.h` | 平滑模式下限制放宽 | 更多并发上传 | ✅ |
| 移除 glFlush() | `RenderDeviceGLES.cpp` | 删除 `glFlush()` | 节省 ~2ms/帧 | ✅ |
| 地形轻量顶点（生产/上传） | 多个文件 | 新增 `TerrainGpuVertex` (32字节) + async 上传 | 带宽减少 73% | ✅ |
| 地形轻量顶点（绘制侧） | `GltfDrawCommandBuilder.cpp` | 32B stride 绘制命令 | — | ❌ 未接通 |
| 地形专用 shader | `Renderer.cpp` | 新增 `kTerrainVertexGLSL/kTerrainFragmentGLSL` + `terrainShader` | GPU 计算减少 60%+ | ❌ 未实现 |

### 修改文件清单

```
scaffold/src/earth_engine/tiling/Tileset.cpp
  └── kSmoothedMainThreadUploadLimit: 1 → 4

scaffold/src/earth_engine/tiling/TileFrameResourceBudgetPlanner.h
  └── maxMainThreadFinalizesPerFrame: 1u → 4u
  └── maxTerminalStateTransitionsPerFrame: 1u → 4u
  └── maxRasterUploadsPerFrame: 4u → 8u

scaffold/src/earth_engine/platform/android/RenderDeviceGLES.cpp
  └── 移除 glFlush()

scaffold/src/earth_engine/tiling/GltfRenderGeometryBuilder.h
  └── 新增 TerrainGpuVertex 结构体 (32 字节)

scaffold/src/earth_engine/tiling/GltfRenderGeometryBuilder.cpp
  └── 新增 buildTerrainVertices() 函数

scaffold/src/earth_engine/tiling/TileRenderContentState.h
  └── GltfPrimitiveRenderResources 添加 useTerrainVertexFormat 标志

scaffold/src/earth_engine/tiling/GltfRenderResourcePreparer.cpp
  └── 检测地形内容并使用 TerrainGpuVertex 格式

scaffold/src/earth_engine/tiling/GltfDrawCommandBuilder.cpp   ← ❌ 计划中，未实现
  └── 设置 32 字节 stride              （未做：仍走 stride-120 makeGltfPrimitiveCommand）
  └── 使用地形 shader                  （未做）
  └── 只绑定必要纹理（base color + 水体遮罩 + 栅格叠加）（未做）

scaffold/src/earth_engine/renderer/Renderer.h
  └── 添加 terrainShader() getter      ← ⚠️ 仅声明（Renderer.h:66），无定义

scaffold/src/earth_engine/renderer/Renderer.cpp   ← ❌ 计划中，未实现
  └── 新增 kTerrainVertexGLSL/kTerrainFragmentGLSL shader   （未做：文件中无此符号）
  └── 添加 terrainShader 成员和初始化                        （未做）
```

> ⚠️ 上面标 ❌ 的三处是本分支的**剩余工作**：地形轻量顶点已能生产并上传到 GPU，但绘制侧未接通（无 32B stride 命令、无地形 shader）。在接通前，走 async 路径的 QM 地形会被以错误 stride 绘制。

---

## 总结

测试全过但实际效果差的根本原因是 **Mock 跳过了所有 GPU 工作**。测试验证了 CPU 侧逻辑正确，但无法验证：

1. **性能特征**（FPS、延迟）
2. **GPU 资源效率**（顶点格式、shader 复杂度）
3. **内存带宽**（纹理上传、顶点数据）
4. **热管理**（GPU 发热 → 降频）

**已修复 / 进行中**：
1. ✅ 主线程上传限制过严（1 个/帧 → 改为 4 个/帧）
2. ✅ glFlush() 阻塞 CPU → GPU 并行（已移除）
3. ⚠️ 地形用 glTF 120 字节顶点 → 32 字节：**生产+上传侧已完成，绘制侧+地形 shader 未完成**（见顶部核对更新）

**预期效果**：
- FPS 从 ~35 提升到 55+
- 地形渲染带宽减少 73%
- GPU 计算量减少 60%+
- 消除卡顿和发烫

**下一步**：
1. 构建并安装 APK 测试
2. 验证 FPS 提升效果
3. 验证地形渲染正确性
4. 检查内存使用情况
5. 如果仍有问题，考虑添加 VAO 缓存（P1）