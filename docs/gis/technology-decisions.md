# 技术栈决策

从 0 开发移动端优先地球引擎前，必须先记录技术栈决策。没有技术路线，AI 很容易在渲染后端、平台桥接、线程模型之间摇摆，导致架构混乱。

## 项目已选定默认路线

本项目采用以下技术栈，决策记录见末尾。

- 语言：C++17（部分 C++20 特性如 `std::span`、`std::expected` 按编译器支持酌情使用）。
- 目标平台：iOS 15+ / Android 8.0+（mobile-first）；桌面（macOS/Windows/Linux）作为开发调试辅助。
- 构建系统：CMake 3.20+，依赖管理通过 vcpkg 或 conan。
- 测试框架：GoogleTest（单元测试 + 集成测试）+ 平台原生截图对比工具（iOS: snapshot testing lib、Android: Roborazzi 或自研）。
- 渲染后端：平台抽象 `RenderDevice` 接口 → iOS: Metal 2、Android: OpenGL ES 3.0 / Vulkan 1.1。
- 数学库：GLM（OpenGL Mathematics）作为首选；Eigen 作为备选（如果需要大规模线性代数）。
- JSON 解析：nlohmann/json。
- HTTP 网络：libcurl + 平台原生桥接（iOS: NSURLSession、Android: OkHttp via JNI）。
- 线程：std::thread + 任务线程池，替代 Web Worker。
- 图片解码：平台原生（iOS: ImageIO/CGImage、Android: BitmapFactory）+ stb_image 作为跨平台回退。
- MVP 模式：只做 3D globe，不做 2D/Columbus view。

这个路线的目标是：用一个 C++ 核心库跨 iOS 和 Android，图形 API 通过 `RenderDevice` 抽象隔离，平台相关 UI、输入、网络、文件系统通过平台桥接层注入。

## 必填决策

每个从 0 开发任务必须填写：

- 目标平台：iOS、Android，还是二者（mobile-first）。
- 语言版本：C++17、C++20 还是 C++23。
- 构建系统：CMake、Bazel、Gradle/native 还是 Xcode project。
- 依赖管理：vcpkg、conan、手动 vendor，还是 monorepo。
- 渲染后端：Metal 2、OpenGL ES 3.0、Vulkan 1.1，还是跨平台中间层（bgfx、sokol、Diligent Engine）。
- 数学库：GLM、Eigen、自研，还是其他。
- 测试框架：GoogleTest、Catch2、doctest。
- 网络库：libcurl、cpp-httplib、平台原生 HTTP API。
- 线程模型：std::thread + 线程池、TBB、平台 DispatchQueue/Looper。
- 图片解码：平台原生、stb_image、libpng/libjpeg-turbo。
- 离线包格式：MBTiles、PMTiles、GeoPackage，还是自定义。
- 日志：spdlog、平台 os_log/logcat，还是自定义。

## 跨平台渲染中间层路线（bgfx / sokol / Diligent）

适合：

- 快速覆盖 iOS + Android，由中间层处理 Metal/Vulkan/GL 差异。
- 团队不想维护多套 shader 变体（bgfx shaderc 自动编译）。
- 3D Tiles/模型加载可借用中间层能力。

风险：

- 地球尺度精度仍需自己处理（camera-relative、origin rebasing）。
- TilePlan、Provider、CRS、数据调度不能交给中间层。
- 深度、透明、picking 和资源释放仍需严格约束。
- 中间层升级可能带来兼容性成本。
- 按条件编译的跨平台 shader 代码和原生 Shading Language 难以直接混用。

## 自研 Metal 2 + OpenGL ES 3.0 路线

适合：

- 想完全掌控渲染管线。
- 目标是长期引擎内核。
- 能投入图形基础设施为每个平台编写 shader 变体（MSL + GLSL ES）。

风险：

- 初期开发慢：两类 shader、两套状态管理、两套资源生命周期。
- 需更强测试和 debug overlay 来保证两个后端行为一致。
- Metal 的资源驱逐（resource eviction）和 GL 上下文丢失策略不同。

## 自研 Vulkan 优先路线

适合：

- 长期面向高端 Android + 桌面。
- 需要 compute、storage buffer 或现代 GPU 管线。

风险：

- iOS 不原生支持 Vulkan（MoltenVK 可桥接但带来开销和兼容性限制）。
- MVP 不应同时承担 Vulkan 学习成本和 GIS 引擎复杂度。
- 移动端 Vulkan 驱动质量差异大，需广泛的设备兼容测试。

## 移动端约束

移动端优先意味着以下约束必须在设计阶段纳入，不能事后补救：

- **电量预算**：持续 60 FPS 渲染对电池消耗极大。引擎应在无交互时降帧率（如 30 FPS 或停止渲染）。避免不必要的 GPU 工作（如无限后处理、过度 MSAA）。
- **发热预算**：长时间高负载会触发系统 thermal throttle。引擎必须在检测到性能下降时自动降低 LOD、减少 draw call、降低纹理分辨率。
- **内存上限**：iOS 和 Android 对应用内存有硬限制（且因设备而异）。GPU 纹理、瓦片缓存、解码缓冲区的总和必须在预算内，并随系统内存压力信号释放非关键资源。
- **存储配额**：离线瓦片包和缓存不能无限增长。必须实现 LRU 淘汰和配额管理。
- **应用生命周期**：iOS 和 Android 随时可能将应用挂起到后台。引擎必须在挂起前保存状态，并在恢复时重建 GPU 资源（尤其是 GL 上下文可能丢失）。
- **网络**：移动网络不稳定，延迟高，带宽有限。瓦片请求必须能处理频繁的网络切换（WiFi↔蜂窝）、DNS 失败和超时。
- **屏幕多变**：刘海屏、安全区域、折叠屏、不同 DPI（1x/2x/3x）、不同宽高比。UI 和 picking 必须对所有屏幕形状正确。
- **GPU 差异**：同一平台的不同设备 GPU 能力差异巨大（从低端 Mali 到高端 Adreno/Apple Silicon）。引擎应在启动时检测 GPU 能力（最大纹理尺寸、扩展支持、性能等级）并自适应配置。

## 决策记录模板

```text
Decision:
  date:
  owner:
  choice:
  alternatives:
  reason:
  consequences:
  revisit condition:
```

重大技术决策必须可追溯，不要只散落在聊天记录里。

## 项目决策记录

### D-001：语言与平台

```text
date: 2025-07
owner: project
choice: C++17，mobile-first（iOS 15+ / Android 8.0+）
alternatives:
  - TypeScript/Web（快速原型，但移动端原生性能不足）
  - Rust（内存安全优秀，但 GIS/图形生态不如 C++ 成熟）
  - Kotlin Multiplatform + Swift（平台原生，但核心引擎无法跨平台共享）
reason:
  核心 GIS 数学、瓦片调度、3D 渲染和数据处理逻辑在 iOS 和 Android 间 100% 共享。
  C++ 是唯一同时被两个平台原生支持（JNI + Objective-C++ bridge）的语言。
consequences:
  - 渲染抽象层（RenderDevice）必须同时支持 Metal 和 OpenGL ES/Vulkan。
  - 构建系统（CMake）需为每个平台生成正确的 native project。
  - 团队需要 C++ 和移动端原生桥接双重能力。
revisit condition:
  当 C++20 modules 被两个平台构建系统稳定支持时，可升级语言版本。
```

### D-002：渲染后端

```text
date: 2025-07
owner: project
choice: 自研 RenderDevice 抽象 + Metal 2 (iOS) + OpenGL ES 3.0 (Android MVP)
alternatives:
  - bgfx（跨平台中间层，开发快但地球尺度精度仍需自研）
  - Vulkan 优先（未来方向，但 MVP 复杂度太高）
  - 只用 Metal + MoltenVK（减少后端但 Android 兼容风险大）
reason:
  Metal 是 iOS 唯一的高效选项；OpenGL ES 3.0 覆盖 Android 95%+ 设备。
  RenderDevice 抽象接口允许以后添加 Vulkan 后端而不改上层代码。
  MVP 不应同时学两个新图形 API。Metal 和 GL ES 3.0 的差异可控。
consequences:
  - shader 需要 MSL 和 GLSL ES 两个版本。
  - RenderDevice 接口不能暴露 Metal 或 GL 特定概念。
  - 灰度测试需要同时在 iOS 和 Android 真机上验证渲染一致性。
revisit condition:
  当 Android Vulkan 设备覆盖率达到 90%+ 且 MoltenVK 在 iOS 上表现可靠时，
  可考虑统一到 Vulkan 后端。
```

### D-003：构建与依赖管理

```text
date: 2025-07
owner: project
choice: CMake 3.20+ + vcpkg
alternatives:
  - Bazel（Google 系，但 GIS 生态依赖的 CMake Find 模块需手动迁移）
  - conan（包版本管理更强，但社区包数量不如 vcpkg）
  - 手动 vendor（控制力最强但更新和维护成本高）
reason:
  CMake 是 C++ 生态的事实标准，所有主要 GIS/图形库（GDAL、PROJ、GLM、libcurl）
  都原生支持 CMake。vcpkg 提供预先构建的 triplets（arm64-ios、arm64-android）。
consequences:
  - CI 需为每个平台 triplet 构建依赖。
  - vcpkg 版本锁定策略需与快速迭代的 C++ 依赖平衡。
revisit condition:
  当 vcpkg 的包滞后成为阻断性问题时，评估 conan 或混合方案。
```

### D-004：数学库

```text
date: 2025-07
owner: project
choice: GLM
alternatives:
  - Eigen（更适合大规模线性代数，但对图形工作流不如 GLM 自然）
  - 自研（可定制但浪费工程资源）
reason:
  GLM 的 API 与 GLSL 一致，图形开发者上手快。覆盖 vec3/mat4/quat 等常用类型。
  如果后续需要大规模矩阵运算（如 bundle adjustment、Kriging），可局部引入 Eigen。
consequences:
  - GLM 默认使用右手坐标系；与 Metal 的左手坐标系转换需显式处理。
  - 地球尺度精度问题仍需 camera-relative 或 double-precision 补充。
revisit condition:
  当分析模块需要 Eigen 特性超过 GLM 能提供时，引入 Eigen 作为补充。
```

### D-005：线程模型

```text
date: 2025-07
owner: project
choice: 自定义任务线程池 + std::thread
alternatives:
  - TBB（功能强大但库体积大，移动端集成复杂）
  - 平台原生（iOS GCD + Android thread pool，核心逻辑无法跨平台共享）
reason:
  引擎核心的瓦片解码、几何解析、纹理上传任务量可控。
  自定义线程池可以根据移动端核心数（通常 2-6 个性能核心）做适配。
  std::thread 是 C++ 标准，零额外依赖。
consequences:
  - 线程池需支持任务优先级和取消（AbortSignal 等价物）。
  - 需实现与平台主循环的集成（不会在后台挂起时执行 GPU 上传）。
  - 不能假设有大量核心可用；默认池大小应保守（2-3 个工作线程）。
revisit condition:
  当需要自动并行化（如 SIMD 矢量运算、大规模几何处理）时，评估 TBB 或 OpenMP。
```
