# 项目全景体检 2026-07-17

> 方法：9 路并行子系统读取（sonnet）→ opus 归并评审（矛盾实地裁决）→ opus 完整性查漏 → 主 agent 复核关键断言。
> 标注"已核实"= 本轮实地 `git`/`grep`/`ctest` 验证；"依据 X"= 信任分报告证据链未复验。

## 0. TL;DR

- **引擎骨架已达生产级**：瓦片选择/加载调度/缓存/QM 地形/上采样均忠实对齐 cesium-native，golden 逐帧字节对拍入库，157 个单测本轮实测全绿（5.83s）。
- **最高风险不是代码而是流程**：2521 个提交堆在三级线性堆叠的本地分支上不进 main，其中两级分支**无远程副本**，零 CI/PR 闸门。修复成本分钟级（push）+ 半天（CI），敞口却是"本地环境损坏 = 丢 35 天工作"。
- **平台不对称是最大产品缺口**：Android/GLES 全功能真机验证；Metal 上天空/大气/雾/FXAA 全是桩（`environment/` 零 MSL），iOS/macOS 只剩纯色背景+地形。
- **崩溃类工程债 4 条**（数据竞争/栈溢出/迭代器 UB/GPU 泄漏），07-05 审计列出后 12 天未修，每条 ≤2h AI 工作量。
- **合规空白**（查漏阶段新增）：demo 硬编码 Cesium ion JWT 已入库、根目录无 LICENSE/NOTICE（本项目是 Apache-2.0 cesium-native 移植 + 内嵌 Mozilla CA）、vcpkg baseline 锁在 2024-01。

---

## 1. 架构地图

**产品形态**：单人 + AI 协作的 C++17 跨平台三维地球渲染引擎，忠实移植 cesium-native 的 3D-Tiles + glTF 瓦片管线，地形走 QuantizedMesh，双后端（Android/GLES + iOS-macOS/Metal）。

**数据流主干**：
```
应用: RenderDevice(平台特定) + PlatformBridge → Engine → EarthEngineSdkFacade(EarthSceneConfig)
     → installScene() → 每帧 facade.update() + engine.render(dt)
Engine::render 四阶段: device->beginFrame() → Scene::update()[相机/环境/Tileset::update 选择+加载+上传]
     → Scene::render()[SceneRenderPipeline 建 RenderCommand 队列] → device->endFrame()
```

**分层规模**（`scaffold/src/earth_engine/`，93,982 行 cpp+h / 95,568 含 mm，537 文件）：

| 层 | 目录(行数) | 职责 | 关键单点 |
|---|---|---|---|
| 瓦片管线 | tiling(31,186) | 选择/遍历/kick-refine/加载调度/缓存驻留/raster映射/glTF上传 | 极致函数式切分：48 Policy 类、66 TileSelection* 文件、83 header-only |
| 内容装载 | content(16,511) | glTF 2.0 解析 + B3DM/I3DM/PNTS/CMPT + QM→glTF + 上采样clip | `GltfModel.cpp` 9045(最大)、`GltfContentProvider.cpp` 4330 |
| 数据源 | providers(13,348) | XYZ/TMS/WMS/WMTS/Bing/Google 影像 + QM/Heightmap 地形 + raster overlay | `RasterOverlayTileProvider.cpp` 4660 |
| 平台 | platform(7,950) | ports-and-adapters：libcurl(Android)/NSURLSession(iOS-mac) + JNI + 内嵌CA | `CaCertBundle.h` 2968(多为PEM) |
| 场景 | scene(5,457) | Scene + 协调器群 + SceneRenderPipeline 绘制顺序/校验 | — |
| 核心 | core(6,739) | geodesy/math/cache/resources/net/async/gltf | `FrameResourceBudget`(本分支改动点) |
| 渲染 | renderer(4,514) | RenderDevice 抽象 + `Renderer.cpp`(2704) + OffscreenPostProcess + GltfUniformBlock 三方契约 | — |
| 矢量后端 | data(1,863) | 新(07-07)：FeatureStore/R-tree/分桶/CDT/线镶嵌 + GeoJSON 导入链 | AI_INDEX 零覆盖 |
| 其他 | layers/environment/interaction/camera/terrain/sdk/style/debug/threading | 图层/大气/输入(含 PickingService)/相机/QM解析/SDK表面 | globe/ = 空目录(历史残留) |

**两后端"故意不同"契约**（新人易踩）：winding 相反（GLES `CCW`/Metal `CW`，抵消 framebuffer 原点差）、离屏 color 格式 RGBA8/BGRA8、v 轴方向 shader 自负、GLES 采样器压到 10 / Metal 完整 20、Metal shader 必须真 MSL 且函数名命中白名单。核心 tile/gltf/terrain/color 四类 shader 严格双源（GLSL+MSL）；**`environment/` 效果层系统性只写 GLSL**（见 §3-9）。

**文档时效**：`AI_INDEX.md` 对骨架仍可信，但自 07-01 未更新，对 `data/` 矢量子系统零覆盖。`docs/issues/` 下多份治理文档已被后续工作推翻（见 §6）。

---

## 2. 子系统成熟度评级

| 子系统 | 评级 | 依据 |
|---|---|---|
| 瓦片选择/遍历/kick-refine | **生产级** | golden 逐帧字节对拍(S1-S4 同步+异步)入库，157 测试全绿(本轮实测) |
| 加载调度/优先级/上传预算/运动降载 | **生产级** | FrameResourceBudget 分 lane 限流；cullRequestsWhileMoving 忠实移植 |
| 缓存/驻留(字节记账+LRU+常驻draw命令) | **生产级** | 三层职责清晰；per-tile 命令缓存不变量有注释护栏 |
| 同步影子树选择(sync-shadow) | **生产级** | 与同步路径逐帧字节一致 |
| 真异步 worker 选择 | **可用** | 遮挡回调恒 nullptr(有运行时 Warning)；raster 读面未镜像(`TileSelectionShadowTree.h:39-45` 自陈) |
| 增量切面(TileIncrementalFrontier) | **半成品(已判 no-go)** | L0-L2 committed；L3 真机测量零收益；L1 捕获层+开关为确认死重 |
| QM 地形解析 + 椭球回落 composite | **生产级** | 逐字节对齐 cesium；`isPureHoleQuad` 纯空洞回落(opt-in 默认关) |
| glTF 内容加载 | **可用(有硬缺口)** | PNG/JPEG 正常；Draco/meshopt/KTX2/Basis **整模型拒收**非降级(已核实，§3-6) |
| 上采样/clip(worker化) | **生产级** | 主线程快照+worker 裁剪，1460ms→18.9ms，逐字节等价 |
| Raster overlay 映射/depot/共享预算 | **可用** | 算法忠实(189 测试)；共享字节账本脆弱+4 路失效组合面大(§3-12) |
| 地形高度采样/相机贴地 | **可用** | 高空早退已修；低空 O(瓦片×三角形)/帧无空间索引(§3-8) |
| 拾取/选择(interaction) | **可用**（首轮误报"未做"，查漏纠正） | `PickingService.cpp`(321 行真实 ray-triangle 求交) 经 `SceneInteractionCoordinator.cpp:10-11` 实例化、`SceneInputCoordinator` 消费；**3D-Tiles per-feature(batch table)级拾取仍未做** |
| 渲染 GLES 后端 | **生产级** | VAO 缓存、离屏 FBO、采样器压缩、真机点亮 |
| 渲染 Metal 后端(核心) | **可用** | tile/gltf/terrain/color 双源正常；环境效果+离屏后处理未接线 |
| 离屏渲染/FXAA/aerial fog | **GLES 可用 / Metal 桩** | `OffscreenPostProcess.cpp:212-217` Metal 分支恒 return false(已核实) |
| 环境(sky/atmosphere/fog) | **GLES 可用 / Metal 桩** | `environment/` 零 backendType 分支，Metal createShader 必失败(已核实) |
| 平台 Android(libcurl+CA+JNI) | **生产级** | 内嵌 Mozilla CA、JNI 线程生命周期、优先级队列 |
| 平台 iOS/macOS(NSURLSession) | **可用** | 网络能跑但忽略请求优先级+无 MSAA+无环境 shader |
| 矢量数据(data/ + layers 接线) | **渲染路径已通 / 编辑核心未起**（首轮低估，查漏纠正） | `VectorLayer.cpp`(594 行)经 `Engine.cpp:307 addVectorLayer` 接线+demo(`MinimalGlobeDemoLayers.cpp`)+GeoJSON 导入链(`GeoJsonImporter/GeoJsonParser`)；P2 编辑核心(EditSession/undo-redo)未起 |
| 手势系统 | **可用** | A0-A2/A5 done；A3/A4/A6 未做；iOS/macOS 输入挂起(用户决策) |
| layers/CreditSystem | **造好未接** | `CreditSystem.cpp` 有实现+单测，scene/sdk 零引用——归属显示(法律义务)卡在这 |
| heightmap 地形抽象 | **死代码** | `HeightmapTerrainProvider`/`TerrainTile`/`TerrainProvider` 无生产实例化(已核实) |
| SurfaceTile 渲染路径 | **死代码** | `RenderCommandKind::SurfaceTile` 无生产者(07-01 Surface Mesh Removal 残留) |
| 测试基建 | **可用但无 CI** | 157 绿+golden 对拍；无任何 CI，mock 跳过 GPU |

---

## 3. 工程风险 Top 12（漏掉代价 × 发生概率，AI 协作基准估工作量）

**1. 分支管理：2521 提交不进 main + 零 CI + 两级分支仅本地** ← Top 1
- 已核实：`main..HEAD`=2521、main 冻结于 `4c9971ccc`(06-12)；三级线性堆叠 `codex/surface-instancing-gpu-batch`(已 push) ← `feat/ellipsoid-fallback-composite-provider`(**仅本地**) ← `feat/offscreen-render-pass`(当前，**仅本地**)；HEAD 内 0 merge commit；无 `.github/workflows`。
- 代价：本地损坏丢 ~35 天工作；线性依赖无独立回退点；2521 提交零合并拦截。
- 工作量：push 两级分支=分钟级；GitHub Actions 跑 ctest+golden=半天；是否合 main 是判断题。

**2. GoogleMapTiles 跨线程无锁数据竞争（UAF/崩溃）**
- 已核实：`GoogleMapTilesImageryProvider.h:109-110` `availableRanges_`/`completeAvailabilityRanges_` 无 mutex（只有 `creditMutex_`:111）；`.cpp:633-642` 写 vs `:757/762` 读并发。~30 分钟。

**3. glTF 节点树无深度上限递归（栈溢出）**
- `GltfModel.cpp:8084 traverseNode`、`:5274 resolveNodeGlobalTransforms` 纯递归无 `kMaxNodeDepth`（依据 docs-debt）。~1-2h。

**4. SharedAssetDepot 缓存 `std::deque::iterator`（潜伏 UB）**
- `SharedAssetDepot.h:189-190` 存 deque 迭代器，push/pop 使其失效（依据 docs-debt）。改 `std::list` 或存 key，~1h。

**5. 环境 pass GPU 资源泄漏（surface 重建）**
- `SkyBox.cpp:229/253`、`AtmosphereBackgroundPass.cpp:317/337` `.release()` 转裸指针，`dispose()` 只置 nullptr。Android 旋屏/前后台切换每次泄漏。~1h。

**6. Draco/meshopt/KTX2/Basis 整模型拒收（非降级）**
- 已核实：`GltfExtensions.h` 白名单缺三项；`GltfModel.cpp:827-855` 对 `extensionsUsed`（非仅 required）任一不支持项整体返 nullptr。大量真实 Cesium ion / 摄影测量数据默认带 Draco/Basis → 拒收。完整解码器集成=多日；临时"跳过非 required 扩展"降级=~半天。

**7. Cesium ion 临时 token 无刷新（长会话地形失效）**
- `EarthEngineSdkFacade.cpp:217-218` token 直拼 URL，仅 installScene 协商一次(:230-238/325-343)，无 401/403 重协商。>1h 会话必现。~半天。

**8. 低空地形高度采样 O(瓦片×三角形)/帧**
- `LoadedTerrainHeightSampler.cpp:310-336` 全瓦片线性扫+全三角形重心测试；早退只覆盖 ≥9050m。近地交互必现。空间索引 ~半天-1 天。

**9. Metal 上天空/大气/FXAA/aerial fog 全不渲染（iOS/macOS 视觉桩）**
- 已核实：`environment/` 零 backendType 分支；`OffscreenPostProcess.cpp:212-217` Metal 恒 false。iOS/macOS 只剩纯色背景。MSL 变体+接线=多日。业务影响随"Android 优先"决策降级。

**10. aerialFog demo 默认开 → 静默丢 4x MSAA 且无 FXAA 兜底**
- 已核实：`MinimalGlobeDemoConfig.cpp:146 aerialFog=true`；离屏 FBO 不支持 samples>1(`RenderDeviceGLES.cpp:514`)；fxaa 默认关。场景被重定向进 1-sample FBO，丢 EGL 4x MSAA，alpha-to-coverage 植被退化。（像素劣化程度未真机 A/B，代码路径推断。）临时"fog 开时同开 FXAA"~1h；正解离屏 MSAA resolve ~1 天。

**11. 影像 provider 全绕过 HttpCache + 无 ETag/Cache-Control**
- XYZ/Bing/Google/TMS/WMS/WMTS 均绕过；`HttpCache.h expiryTime` 从不赋值（依据 docs-debt）。LRU 逐出即全量重下、重启不留缓存。~1 天。

**12. Raster overlay 共享字节账本脆弱 + 4 路失效组合面**
- `sharedRasterImageRefs` 双引用计数手动配平(`RasterOverlayTileProvider.cpp:2523-2661`)；`invalidateMapped/Direct/SourceAssetDepot + abandonActiveSourceSets` 四路互调(:3049-3207)，近 6 提交都在补此组合面。治理：显式状态图+护栏 ~半天。

**荣誉提名**（不占额）：SurfaceTile 死代码链 shader 编译失败会拖垮 `Renderer::initialize()`(`Renderer.cpp:2456-2460`)；四个上帝文件持续增长(GltfModel 9045/RasterOverlayTileProvider 4660/GltfContentProvider 4330/Renderer 2704)；WMS 硬编码 `crs=EPSG:4326` 不随 version 切(`WebMapServiceImageryProvider.cpp:311`)；iOS/macOS 忽略 `HttpRequestOptions.priority`。

### 3b. 安全与合规（查漏阶段新增，Top 12 之外单列）

- **S1 凭证入库**：`scaffold/examples/android/MinimalGlobe/MinimalGlobeDemoConfig.h:18-22` 硬编码 Cesium ion JWT（iat≈2021 公开 demo token，大概率已失效但确属入库凭证）；同文件高德模板走明文 `http://`。将来开源/上架前必须清理并轮换。
- **S2 无 LICENSE/NOTICE**：`git ls-files` 根级零 LICENSE。本项目是 cesium-native(Apache-2.0) 忠实移植 + 内嵌 Mozilla CA(`CaCertBundle.h`) + vendored GLM/nlohmann/stb —— 均带归属义务。"法律上能不能 ship"目前无答案。~半天补齐。
- **S3 依赖供应链**：`scaffold/vcpkg.json` 锁 `builtin-baseline 2024.01.12`（curl>=8.0.0/glm/nlohmann-json/gtest/stb），距今 2 年+，curl/OpenSSL CVE 敞口未评估。升 baseline+回归 ~半天。

---

## 4. 未完成工作清单

### D1. 当前分支 WIP（12 文件未提交，+727/−15，已核实）
- 内容：raster overlay **"早映射"预算车道** —— `FrameResourceBudget`(+53) 新增 `tryStartRasterOverlayMapping/canStartRasterOverlayMapping/recordRasterOverlayMappingElapsed`（次数 4/帧 + 耗时 1ms/帧双预算）；`TileRasterOverlayFrameProcessor.h`(+107) 新增 `tryEarlyMapping`（允许 not-Done 瓦片在独立预算下提前建映射，部分回退闸1）；debug log 链路配套；测试 +501 行。
- 状态：与分支名 offscreen-render-pass 无关（主题漂移）；仅单测覆盖，早映射默认值(4次/1ms)未见真机验证记录。
- 待决：是否拆分单独提交；默认值真机验证后再定。

### D2. 历史审计遗留 P1（07-05 技术债审计，12 天未修，本轮抽验仍 OPEN）
- 正确性 4 条 = §3 的 2/3/4/5（数据竞争/栈溢出/迭代器 UB/GPU 泄漏）。
- 性能 3 条：每帧每 primitive 深拷贝 RenderCommand(`GltfDrawCommandBuilder.cpp:458`)、上采样白拷父地形顶点(`GltfTerrainUpsampler.cpp:921`+`TileGltfTerrainUpsampledChildMaterializer.h:88`)、SceneRenderPipeline 每帧 7-8 遍全量遍历。
- 均"<1h~半天，手术式"，一直未排期。
- **纠正**：审计所列 `GltfRenderResourcePreparer::prepareCpuWork ~230 行死代码`经本轮核实**有真实调用点**（`TilesetContentLifecycleCoordinator.h:198`、`GltfRenderResourcePreparer.cpp:361`），**不应清理**。

### D3. 平台性能审计 P0/P1（07-04）——已修可信
- P0-1~4、P1-1~10 全部有 commit 号且源码抽查一致。仅 P2 约 5 条未修（iOS 桥优先级/纹理池化/submit 尾部状态拆除/Metal 状态去重/overlay revision 门控）。

### D4. 设计已成文未落地
- **矢量数据系统**：P0/P1a/P1b/P1c committed；P2 编辑核心（EditSession/undo-redo/编辑手柄/snap/脏区增量重镶嵌）未起。设计文档 §7.1 "createFramebuffer 返 nullptr"前提已被 offscreen 工作推翻**未回写**；`Framebuffer`/`RenderCommand` 仍无 stencil attachment，贴地方案 B 未解锁。
- **椭球回落**：已落地(opt-in 默认关)；§8 父终止空洞缺口休眠，需真实 partial QM 数据集验证。
- **LOD cross-fade**：做完但因黑块 bug 默认关(`enableLodTransitionPeriod=false`)。
- **未做大件**：真 Cook-Torrance BRDF、HTTP/2、在途请求取消、预测预取、per-feature 拾取、3D Tiles 样式语言、CSM 阴影、HDR/tone-mapping、geomorph、GetCapabilities 解析、CreditSystem 接线。

### D5. 测试基建缺口
- 无 CI（已核实）；无 ANGLE 真实 shader 编译验证；无真机集成测试；MockRenderDevice 跳过 GPU。注意：`test_sse_pipeline` 是 27,900 行/488 个手写 `testXxx()` 的自研断言框架，CTest 只报整体 pass/fail。

### D6. Metal/iOS/macOS 平台补齐
- Metal swapchain MSAA、FXAA/aerial fog MSL、环境 shader（sky/atmosphere）——取决于是否要求这两平台做 GE 观感（用户拍板）。

---

## 5. 平台可运行矩阵

| 平台 | 状态 | 证据 |
|---|---|---|
| Android (GLES/Adreno 730) | **全功能真机验证** | terrain+imagery+fog+sky+光照，多轮真机测量记录 |
| macOS (Metal) | **核心地形出图（07-01 验证），无环境/后处理** | 纯色背景+地形；07-01 后未复验 |
| iOS (Metal) | **适配器存在，从未验证 build/run** | `IosPlatformBridge` 已接线，无运行记录 |

单测全绿 ≠ 平台可跑：mock 跳过 GPU，唯一运行时证据来自真机记录。

---

## 6. 本次体检对既往结论/文档的修正

1. `prepareCpuWork` **不是死代码**（07-05 审计误判，有 2 处真实调用点），勿清理。
2. **双轴路线图(07-08)已过时**：U1 "createFramebuffer 返 nullptr" 已被两后端真实现推翻（`RenderDeviceGLES.cpp:503`/`RenderDeviceMetal.mm:613`）；QW1 ambient 已接线（`Renderer.cpp:1045-1055` 半球模型，07-10）；QW3 water-mask 已是真实 `mix(base.rgb, waterRgb, water)`（`Renderer.cpp:368/986`）——三条本轮主 agent 亲验。QW2 swapchain MSAA 仍只欠 Metal 侧。
3. 矢量设计文档 §7.1 的 RTT 封死前提已失效，未回写。
4. "per-feature 拾取未做"表述需精化：**拾取服务已存在并接线**（ray-triangle），未做的是 3D-Tiles batch-table 级 feature 拾取。
5. `AI_INDEX.md` 对 `data/` 零覆盖，自 07-01 未更新。
6. `build/native-tests` 下有 4 个孤立测试二进制（test_globe/test_render_command_streaming_set/test_scene_terrain_transition_gate/test_tile_surface，源码已删产物未清），排查时易误导。
7. 仓库卫生（低危不占风险位）：已入库垃圾 = `click-8.1.8-py3-none-any.whl`、`glm-0.1.0.tar.gz`、`tmp/` 73 个调试文件(~3MB)、10 个 `.run_output*.log`、`scaffold/check_png*.py`+`screenshot*.png`（含损坏文件 `screenshot2.png;`）、`Testing/Temporary/` CTest 产物。清理=`git rm`+补 .gitignore，~半小时。

---

## 7. 建议下一步（按敞口/成本比排序）

1. **立即**：push `feat/ellipsoid-fallback-composite-provider` 和 `feat/offscreen-render-pass` 到 origin（分钟级，消掉最大数据丢失面）。
2. **本周**：修 4 条崩溃类 P1（§3-2/3/4/5，合计 ≤1 天）；搭最小 CI（ctest+golden，半天）。
3. **ship 前必清三件事**：S1 凭证轮换+移出源码、S2 LICENSE/NOTICE、iOS/macOS 冒烟验证。
4. **判断题**（用户拍板）：main 合并策略；Metal 平台是否补齐 GE 观感（D6）；Draco/KTX2 是"接真实数据"的前置（§3-6），若近期要接 ion 3D-Tiles 建筑数据则优先级提前。
