# 架构沉淀 (docs/architecture/)

面向**未来接入新功能**的架构参考:每个核心子系统"怎么搭的、关键决策为什么这么定、诚实得失、接新功能从哪切入"。

## 这是第五类文档,别和其它四类混

本仓文档体系此前有四类,各答一个问题。这套 `docs/architecture/` 是新增的第五类:

| 文档 | 回答 | 性质 |
|---|---|---|
| `AI_INDEX.md` | 代码在哪(结构/行号) | 有 ctest 守卫,行号漂移会红 |
| `docs/northstar/*` | 做到什么程度算好 | 活文档,随专项收官更新(判据 V/P 编号=跨会话锚点) |
| `docs/issues/*` | 当时怎么修的 | 写完即冻结的历史事故档 |
| `README.md`(仓库根) | 仓库入口地图 | 面向人的导航 |
| **`docs/architecture/*`** | **这个子系统怎么搭的、为什么、接新功能从哪切** | **设计说明** |

前四类都不覆盖"设计理由 + 诚实得失 + 扩展点"。写代码前想接入某模块,先读这里对应那份;想知道"某判据是否达成"去 northstar;想知道"某符号在哪一行"去 AI_INDEX。

> ⚠️ 本目录的行号锚点**没有 ctest 守卫**(不像 AI_INDEX),会随重构漂移——**一律以符号名为准,行号只作路标**。得失/债的权威引用点仍是 northstar 的 V/P 编号。

## 子系统总图

```
Engine (生命周期外壳 + 输入路由)
  └── Scene (自研组合根, 5 Coordinator + 渲染管线)          → core-scene.md
        ├── Tileset(s) 瓦片调度 [引擎重心, 34k 行]            → tiling.md ★
        │     ├── TerrainProvider → 地形内容                  → terrain.md ★
        │     ├── ImageryProvider → 影像/栅格叠加             → imagery.md
        │     └── (选择/加载/缓存/GPU上传, 对齐 cesium-native)
        ├── VectorLayer 矢量 (FeatureStore 编辑层 + MVT 底图) → vector.md
        ├── Camera/CameraSystem + Interaction 相机与手势      → camera-interaction.md
        ├── Environment 光照/天空/大气                        → environment.md
        └── Renderer + RenderDevice 渲染 [引擎重心, 双后端]   → renderer.md ★
              ├── RenderDeviceGLES (GLSL ES 3.0 / Android)
              └── RenderDeviceMetal (MSL / iOS·macOS)

core/ 地基 (数学/大地测量/异步/缓存/网络)                     → core-scene.md
```
★ = 引擎重心三大件,写得最深。

## 八份文档

| 文档 | 覆盖 | 一句话定性 |
|---|---|---|
| [tiling.md](tiling.md) ★ | 瓦片选择/遍历/LOD/调度/GPU上传/缓存 | 逐算法对齐 cesium-native,极端小类分解;优先级在两处被架空 |
| [terrain.md](terrain.md) ★ | 地形内容/网格/GPU位移/无缝/高程服务 | 共享模板+GPU纹理位移是亮点;几何密度钉死粗一个数量级 |
| [renderer.md](renderer.md) ★ | RenderCommand/双后端/契约头/页存储 | 命令校验+编译期契约治理强;Metal 后端结构性缺口 |
| [imagery.md](imagery.md) | 栅格叠加映射/多协议 provider/GCJ-02 | 对齐 cesium RasterOverlay + 自研 GCJ 偏移系;HttpCache 无重验 |
| [vector.md](vector.md) | FeatureStore 编辑层 + MVT 三分工底图 | 表示随负载三分工是亮点;样式系统割裂三套 |
| [camera-interaction.md](camera-interaction.md) | 控制器/约束求解/手势/拾取 | 真值按控制器分离 + 单一钳位出口;手势无外部对照系 |
| [environment.md](environment.md) | 时间→太阳→天空/大气/日落着色/HDR | 天空↔雾单一治理点;两套天空模型 + HDR 半成品挂起 |
| [core-scene.md](core-scene.md) | 数学/大地测量/异步/缓存 + 场景装配 | 大地测量有 cesium 对照测试守卫;WorkLedger 尚未接管 gating |

## 贯穿全引擎的设计取向(读任何一份前先建立这层认知)

1. **混血血统**:tiling/streaming/大地测量/数学**对齐 cesium-native**(对照实现的测试即行为规格);camera/interaction/environment**对齐 openglobus**。血统决定了"改这块该去翻哪个参考实现的测试"。
2. **表示随负载,不是一套渲染打天下**:地形(GPU 位移网格)、矢量面(drape 栅格化)、矢量线(D2 场解析)、矢量点(billboard)各用完全不同的表示——判据是"体验达标且最便宜",不是架构统一。代价是这些路代码几乎不共享。
3. **契约机器可查化**:跨子系统的调用顺序、深度/绕序约定、命令排序、在途账本这些"编译期类型系统表达不了"的约束,用运行期 `contracts::Id` 断言 + 编译期 `static_assert` + ctest 守卫补足。多份文档的"关键契约"节列的就是这些。
4. **失败方向工程化**:反复出现的模式是"把默认失效方向从最坏(静默冻屏/静默采样 0/静默分叉)反转为需要故意为之才会坏"(WorkLedger、RenderCommand 校验、失败安全默认值)。
5. **诚实的债文化**:northstar 的 P 编号债表 + "已判死/勿再提"节 + memory 的教训沉淀,共同防止重复踩坑与重复提议错方案。本目录的"诚实得失"节从这些来源提取,**不美化**。

## 跨模块诚实总账(挑最该知道的)

接新功能前值得先知道的系统级短板(细节见各文档):

- **Metal 后端系统性滞后**:离屏后处理链、stencil 贴地分类、地形 GPU 位移/烘焙、矢量场解算 MSL、HDR 终端 pass 在 Metal 上或缺失或未真机验证。跨平台功能不要假设 Metal 与 GLES 等价。(renderer/terrain/vector/environment)
- **优先级信号多处被架空**:`FrameResourcePriority` 在预算门控被静默忽略、`GpuUploadQueue` 是 FIFO 非优先级序——高 SSE 瓦片不能优先上屏。(tiling)
- **shader 无 host 执行级守卫**:GLSL/MSL 是 C++ 字符串运行时才编译,host ctest 抓不到,唯一验证途径是真机肉眼——已多次导致 GPU/CPU 实现静默分叉。(terrain T-P6、environment L-P4、vector)
- **样式系统割裂三套**:矢量要做运行期换肤/热加载需同时改三处。(vector)
- **HttpCache 无过期/无 ETag 重验**,缺 cesium 式请求侧护栏(按屏幕优先级重排 + 相机移走 cancel)。(imagery/core-scene)
- **手势系统无外部对照系**,核心判据 anchorErr 的常驻探针已被移除,排查需先重新插桩。(camera-interaction)
- **WorkLedger 尚未真正接管 gating**(过渡态),旧的"四判据各自为政"风险敞口在 gating 层面尚未消除。(core-scene)

## 维护约定

- 专项收官、架构发生变化时,更新对应那份的"核心设计决策"与"诚实得失"节。
- 得失/债的**判定与编号**归 northstar(V/P 锚点);本目录只做"设计说明"的归并引用,不自建判据编号。
- 新增子系统 → 照现有八份的统一模板加一份(职责边界 / 核心设计决策+理由 / 数据流 / 关键契约 / 诚实得失 / 扩展点 / 对照系),并在本 README 的总图与表格里挂上。
