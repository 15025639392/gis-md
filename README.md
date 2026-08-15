# earth-engine (gis-md)

**C++17 移动优先的 3D 地球引擎**——自研内核，不套 Cesium/osgEarth，只把它们当算法对照系。
目标形态是一个可嵌入 App 的 `earth_engine_core` 静态库 + 各平台薄壳 demo：
球面地形流式加载、影像瓦片合成、运行期可换样式的矢量图层、手势相机。

- 语言/标准：C++17（Metal 桥接部分为 Objective-C++）
- 后端：**GLES 3**（Android，主力）/ **Metal**（iOS / macOS，追平中）
- 依赖：glm、nlohmann-json、curl、stb、gtest（vcpkg 清单模式，见 `scaffold/vcpkg.json`）
- 规模：核心库 ~16 万行 / 16 个模块；186 个 native 单测；3000+ commit

---

## 快速上手

所有构建入口都在 `scaffold/`。**先 `source env.sh`**——它固定了本机 NDK / CMake / Ninja / JDK / vcpkg 路径，绕过它会到处找不到工具。

```bash
cd scaffold && source env.sh
```

### 跑单测（最短反馈回路）

```bash
cd scaffold && ./test_native.sh
```

```bash
cd scaffold && ./test_native.sh --ctest -R Rectangle
```

```bash
cd scaffold && ./test_native.sh test_camera_system
```

`test_native.sh` 自己会 `source env.sh` 并按 `native-tests` preset 配置构建，带并发锁（多会话同时跑不会互相踩）。

### 构建 Android demo

```bash
cd scaffold && ./build_android.sh
```

产物是 `examples/android` 下的 `MinimalGlobe` demo，包名 `com.earthengine.minimalglobe`。真机跑之前注意两个反复踩过的坑：

- **`adb reverse` 必须设**，否则本地瓦片服务不通，表现是「矢量/影像全灭」，看着像代码 bug。USB 重连后会静默失效。
- **装 debug 变体**，release 变体在部分机型（ColorOS）上 logcat 什么都不吐。

### CMake presets

| preset | 用途 |
|---|---|
| `native` | 本机 Debug，不带测试 |
| `native-tests` | 本机 Debug + `BUILD_TESTING=ON`（`test_native.sh` 用这个） |
| `android-arm64` | Android arm64-v8a，需 `ANDROID_NDK_HOME` |

`BUILD_EXAMPLES=ON` 才会构建 `examples/`（macOS / iOS 走 CMake，Android 走 Gradle）。

⚠️ **性能数字一律在 release 构建上取**，debug 构建的帧时没有参考价值。

---

## 目录地图

```
gis-md/
├── AI_INDEX.md          # 代码索引：哪个算法在哪个文件哪一行（有 ctest 守卫）
├── CLAUDE.md            # 项目级 AI 工作规则（北极星文档协议）
├── AGENTS.md            # 算法对齐规则（cesium-native / openglobus 分工）
├── docs/
│   ├── northstar/       # ★ 活文档：每个模块「做到什么程度算好、现在到哪了」
│   ├── issues/          # 专项档案：当时怎么修的（写完即冻结）
│   └── gis/             # GIS 领域知识库（CRS / 投影 / 瓦片方案 / 算法约定）
└── scaffold/
    ├── src/earth_engine/  # 核心库
    ├── tests/unit/        # 186 个 gtest 单测，按模块分目录
    ├── examples/          # android / ios / macos 三个 MinimalGlobe demo
    └── tools/             # 诊断台：A/B 判定、裂缝度量、AI_INDEX 守卫等
```

### 核心库模块（`scaffold/src/earth_engine/`）

| 模块 | 行数 | 职责 |
|---|---:|---|
| `tiling/` | 34k | 瓦片树 / LOD 选择 / SSE / 调度 / 页存储合成——**引擎的重心** |
| `content/` | 17k | 地形内容：heightmap / glTF / 上采样 / 位移模板 |
| `renderer/` | 12k | 渲染命令、地形页存储、深度 prepass、图集、离屏后处理 |
| `providers/` | 11.5k | 数据源：XYZ / WMTS / WMS / TMS / Bing / Google / 矢量 drape |
| `platform/` | 9.7k | RenderDevice 后端（GLES / Metal）+ curl 网络桥 |
| `core/` | 7.3k | 数学、几何、大地测量（椭球 / 投影 / GCJ-02）、异步、缓存 |
| `scene/` | 7k | 场景图、帧状态、诊断信号 |
| `data/` | 6.2k | 矢量：GeoJSON / MVT 解码、三角化、样式表达式、要素索引 |
| `layers/` | 5.1k | 图层组织与要素渲染层 |
| `camera/` | 3.7k | 相机系统、约束求解、飞行 |
| `environment/` | 1.6k | 天空 / 大气雾 / 光照 |
| `interaction/` | 1.4k | 手势管线（拖拽 / 双指 / 惯性 / anchor） |
| `debug/` `sdk/` `style/` `threading/` | 2.6k | 诊断 overlay、SDK facade、样式、线程原语 |

顶层 API 是 `Engine`（`Engine.h`）：平台代码建 `RenderDevice` → 建 `Engine` → `onSurfaceCreated/Changed` → 每帧 `render()` → `onInputEvent()` 转发输入。
场景装配走 `sdk/EarthEngineSdkFacade`（从 `EarthSceneConfig` 一次性装地形 + 影像 overlay + 初始相机）。

---

## 文档体系：三类文档，别混

这是本仓最需要先理解的一件事。三份文档回答**三个不同的问题**：

| 文档 | 回答 | 性质 |
|---|---|---|
| `AI_INDEX.md` | 代码在哪（结构 / 行号） | 有 ctest 守卫，行号漂移会红 |
| `docs/issues/*` | 当时怎么修的 | 写完即冻结的历史档案 |
| **`docs/northstar/*`** | **做到什么程度算好** | **活文档，随专项收官更新** |

### 北极星文档（`docs/northstar/`）

现有 `vector.md` / `terrain.md` / `lighting.md` / `imagery.md`。每份用一张判据表钉住体验目标：

- **判据编号是跨会话稳定锚点**，只增不改。说「V5 不满意」即指该条，不必重新描述观感。
  命名空间：vector 用无前缀 `V*`/`P*`（历史既有），terrain 用 `T-*`，lighting 用 `L-*`，imagery 用 `I-*`。
- **状态**：✅ 达成（有证据）/ ⚠️ 有缺口 / ❌ 未做 / 🔒 待拍板。**改状态必须附证据**（commit / 真机数据 / 截图），不许凭印象改。
- **类型决定谁说了算**：【机制】类由命令输出、计数、测试红绿自证；【观感】类**像素判断归用户**。
- **「代价」列是硬要求**：每条体验写清花了多少 GPU/CPU/内存。没量化就写「未量化」，**不许填"应该很小"**——空头承诺会沉淀成债。
- 每份还带一节**「已判死 / 勿再提」**，附死因，防止日后重复提议同一个错方案。

动手改任何**用户可见行为**（渲染观感、交互手感、性能特征）之前先读对应那份，并在动手前说明「本次动 V 几」。纯内部重构 / 构建脚本 / 测试工具不需要。

---

## 算法对齐

引擎算法以 **cesium-native** 为主要对照系（坐标、瓦片、地形、相机、拾取、LOD、Provider、裁剪、投影、椭球、Quantized Mesh、SSE、包围体）；交互与表现层以 **OpenGlobus** 为补充对照系。

关键约定：**对照实现的测试是行为规格的一部分**——对齐某个算法时，从 cesium-native 对应的 `test/Test*.cpp` 提取输入、期望输出、边界条件、数值容差，先转写 case 再改实现。

手势系统**没有**外部对照目标，交互契约在本仓内自己定义并测试。

细则见 `AGENTS.md`。

---

## 工具台（`scaffold/tools/`）

| 工具 | 用途 |
|---|---|
| `load_ab/` | A/B 判定台——**超阈值但 p ≥ 0.05 判 INCONC**，不允许拿单次 run 下结论 |
| `seam_metric/` `seam_line_detect.py` | 地形裂缝 / 瓦界错缝的像素级度量 |
| `selector_diff/` | LOD 选择器决策差分 |
| `check_ai_index_refs.py` | `AI_INDEX.md` 行号引用一致性守卫（进了 ctest） |
| `check_pipeline_feature_contracts.py` | 渲染管线契约守卫（进了 ctest） |
| `mvt_demo/` | 本地 MVT 瓦片服务，喂矢量底图 demo |

设备上的帧时噪声可达 ±2×（DVFS 调频），所以 **A/B 优先比计数类指标**，不要只比帧时。

---

## 当前状态速览

各模块的准确进度以 `docs/northstar/*` 的判据表为准，下面只是粗略定位：

- **地形**：无缝已收官；GPU 位移 + 共享网格已落地。短板在几何密度（65×65 钉死）与光照动态范围压平了 relief（`terrain.md` T-V1/T-V3）。
- **矢量**：全链打通（MVT 底图 / SDF 标注 / 贴地线面 / 样式表达式）。线宽像素一致专项已换代收官，形态定在线段纹素距离场（`vector.md`）。
- **影像**：空洞退化走祖先影像不留白；GCJ-02 偏移瓦片已接入并根修过缩放过渡错位（`imagery.md`）。
- **光照/颜色**：LDR 下日落暖化已生效；HDR 线性管线内容补齐但**默认关、仅 GLES、常数未调**。主要是结构债——同一件事写了多套且零测试兜底（`lighting.md` L-P1~L-P4）。
- **平台**：GLES 是主力且功能最全；**Metal 后端在追平**，部分特性（如 stencil 相关的贴地路径）尚未对齐。
