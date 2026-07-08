# 地球引擎双轴路线图 · 完整 Checklist

**日期**: 2026-07-08
**目的**: 把两条正交的成熟度轴合并成一张可勾选地图 —— **X 轴 = 引擎保真度**(离 cesium-native 多远)、**Y 轴 = 平台能力**(离"地球 Online / 活星球"多远)。前者补债,后者选赛道。
**基准**: 工程量按 **AI 协作执行**估(非人手工时);性能判断以 **release(-O2)** 为准。
**证据来源**: `tile-render-pipeline-gap-audit-2026-07-06.md`、`platform-independent-tech-debt-audit-2026-07-05.md`、`viewport-tile-load-speed-research-2026-07-06.md`、`gesture-system-audit-2026-07-06.md`、`test-coverage-gaps.md`、各设计文档 + memory 归档。

---

## 图例

**工程量(AI 协作基准)**: `S` <0.5天 · `M` 0.5–2天 · `L` 3–7天 · `XL` 1–3周+
**严重度**: `P1` 关键 · `P2` 重要 · `P3` 应做 · `P4` 锦上添花
**状态**: `[ ]` 待办 · `[x]` 已完成 · `[~]` 进行中 · `[!]` NO-GO(勿做)

> **总体定性**: 无 P0 阻断项。X 轴是"能跑、能出正确帧的引擎离成熟有多远";Y 轴是"一个播放器离一个平台有多远"。两轴投入逻辑不同,**不必也不可能全做** —— 先定"这颗地球要成为什么",再沿一条 Y 轴赛道推进,X 轴按杠杆补债。

---

## ⚡ 关键解锁点(先看依赖)

三个"单点解锁一大片"的地基,优先级独立于梯队:

| ID | 解锁点 | 解锁了什么 | 工程量 |
|----|--------|-----------|--------|
| **U1** | `createFramebuffer` 实现(两后端现为 stub 返 nullptr) | **AA / HDR / tone-mapping / bloom / 阴影(CSM) / 全局光照 / 任何后处理** 全部 | M |
| **U2** | HTTP 响应头管线(拓宽 `PlatformBridge` 回调带 header) | 退避重试(Retry-After)、缓存重验(ETag/304/Cache-Control)、内容协商 | M |
| **U3** | WebGPU/Vulkan compute 后端(现 Metal/GLES) | 体积云、FFT 海洋、实时 GI、光追、3DGS、GPU 分析算子 —— 整个渲染前沿 | XL |

---

# X 轴 · 引擎保真度(补债)

## 梯队一 — 速赢(plumbing 已铺,接线即见效)

- [ ] `QW1` **P1 · S** — 接线 ambient 环境光:`SkyGradient::ambientColor()` 已算好但零处接线 → 灌进 `u_ambientColor`(地形+glTF 共用),背光面死黑立即消失
- [ ] `QW2` **P1 · S** — swapchain 层开 MSAA:Metal `rasterSampleCount` + GLES `EGL_SAMPLES`(无需 offscreen),边缘爬行立即消失
- [ ] `QW3` **P1 · S** — 实现水面着色:整条 QM water-mask 解码上传却是 `mix(base,base,water)` 死 no-op → mask 门控水色+高光
- [ ] `QW4` **P2 · S** — 默认开 LOD cross-fade:`enableLodTransitionPeriod=true`(链路已内建)
- [ ] `QW5` **P1 · S** — WMS version 分支:轴序硬编码 1.3.0,请求 1.1.1 会畸形 → 按 `options_.version` 切 srs/crs + BBOX 轴序
- [ ] `QW6` **P1 · S** — 退避重试(依赖 U2 拿 Retry-After,或先做纯本地指数退避):`FailedTemporarily` 每帧重打服务器 = 自造 DoS
- [ ] `QW7` **P1 · M** — 放宽 GPU 上传/finalize 每帧预算(默认 1,卡死"下载完→显示"延迟)+ 骨架瓦片不计预算(osgEarth 式);须 release 实测

## 梯队二 — 核心保真/观感(中成本)

- [ ] `CF1` **P1 · M** — 真 Cook-Torrance BRDF:现为 Blinn-Phong 近似,PBR 输入已 plumb(甚至超 cesium 覆盖),差在求值质量
- [ ] `CF2` **P1 · M** — 地表 ground atmosphere + aerial perspective/fog:**vs cesium/GE 最大观感差距**。`AtmosphereBackgroundPass` discard 所有朝地射线却指望地表 shader 接手,而地表 shader 无大气代码 → "贴平图"感的根源
- [ ] `CF3` **P1 · M** — HTTP 头管线 + 缓存重验(= U2):响应头被结构性丢弃,每瓦片永久缓存永不重验
- [ ] `CF4` **P1 · M** — 影像走统一缓存:XYZ/Bing/Google/TMS/WMS 全绕过 HTTP/磁盘缓存,LRU 逐出即全量重下,重启不留存
- [ ] `CF5` **P1 · L** — Draco + meshopt + KTX2/Basis 解码(KTX2 现被主动拒绝):**最高杠杆功能项**,不解则大批真实生产 3D Tiles 加载失败/无纹理
- [ ] `CF6` **P1 · M** — per-feature 拾取:batch table + EXT_mesh_features 已解析从不暴露,`PickResult` 无 feature/batchId
- [ ] `CF7` **P2 · M** — 地形逐顶点法线 PBR + geomorph:oct 法线已解析上传却只喂一个 clamp lobe;无 LOD 几何形变会 pop

## 梯队三 — 生产功能(较大工程,按产品需要)

- [ ] `PF1` **P1 · M** — 3D Tiles 样式语言:无表达式求值器,无法按属性 color/show/hide(数据可视化核心)
- [ ] `PF2` **P1 · L** — implicit tiling / subtree(3DT 1.1):很多现代瓦片集只发 implicit
- [ ] `PF3` **P2 · M** — clipping planes + 分类(classification)
- [ ] `PF4` **P2 · M** — HDR + tone-mapping(ACES) + bloom(依赖 U1)
- [ ] `PF5` **P2 · L** — 阴影(CSM / 地形自阴影,依赖 U1)
- [ ] `PF6` **P2 · M** — 影像色彩调整(gamma/亮度/对比/饱和/色相)+ SingleTile/ion imagery 叠加类型 + Bing imagerySet 选择
- [ ] `PF7` **P2 · M** — CreditSystem 接线(覆盖区感知归属,法律/署名)+ WMS/WMTS GetCapabilities 解析
- [ ] `PF8` **P3 · L** — HTTP/2 多路复用(`CurlMultiRequestScheduler` 现全局 20 上限无 host 分桶)

## 梯队四 — 韧性 / 正确性(07-05 审计 P1,全部 OPEN)

- [ ] `CX1` **P1 · S** — GoogleMapTiles availability 向量加锁:无锁跨线程读写 = **UAF,首个 Google 影像视口可能崩**
- [ ] `CX2` **P1 · S** — SkyBox/AtmospherePass `.release()` 裸指针 → 用 unique_ptr(每次 onSurfaceCreated 泄漏 shader+buffer)
- [ ] `CX3` **P1 · S** — glTF 节点树 / tileset children 无界递归 → 限深防栈溢出(敌意输入 SIGSEGV)
- [ ] `CX4` **P1 · S** — `SharedAssetDepot` 缓存 deque::iterator(UB)→ 改 `std::list`;`put` 已存在 key 只加不减字节
- [ ] `CX5` **P1 · S** — `FrameResourcePriority` 在预算里被静默忽略 → urgent 可被 preload 饿死
- [ ] `CX6` **P1 · M** — RenderCommand 每帧每 primitive 深拷(sizeof=1664,带 string+vector)→ 轻量化 handle 化
- [ ] `CX7` **P1 · S** — 上采样深拷整个父地形再丢弃 primitive → 只拷元数据
- [ ] `CX8` **P1 · S** — `SceneRenderPipeline` 对命令列表 7-8 次全遍历 + 未 gate 诊断 pass → 折叠 + 门控
- [ ] `CX9` **P2 · S** — epoch-mismatch 分支在池线程直接写 `tile->setState(Failed)`,违反"state 只主线程写"→ 走 pendingUpload 主线程终结
- [ ] `CX10` **P2 · S** — GLES 忽略 `blendSrc/blendDst` 硬编 → 加性混合内容在 Android 渲错
- [ ] `CX11` **P2 · S** — TMS/config 畸形 XML 抛异常无 try/catch → 中止 init;RGB→RGBA blit stride 越界读
- [ ] `CX12` **P2 · S** — QM 请求加 `Accept: application/vnd.quantized-mesh;extensions=…` 头 + content-type 校验(走 Accept 协商的 ion 会静默降级)

## 梯队五 — 测试基建(回归防线,ROI 随成熟度上升)

- [ ] `TS1` **P1 · M** — shader 编译验证测试(CI 引 ANGLE):当前无编译期护栏
- [ ] `TS2` **P1 · S** — 运行时纹理单元上限检查(`GL_MAX_TEXTURE_IMAGE_UNITS`):正是 Adreno 黑屏那类真机 bug 的护栏
- [ ] `TS3` **P1 · L** — 真机集成测试(Espresso + 截图校验 + logcat 断言)
- [ ] `TS4` **P2 · M** — 端到端渲染管线测试(软件渲染器 + 命令生成校验)

## 梯队六 — 工程卫生(行为中性,随时可做)

- [ ] `HY1` **— · M** — god-file 机械 TU 拆分(GltfModel 8981 / GltfContentProvider 4267 / RasterOverlayTileProvider 3724 / Renderer 2622 行)
- [ ] `HY2` **— · S** — `tiling/` 279 扁平文件分子目录
- [ ] `HY3` **— · M** — 下沉 Camera/Frustum/FrameState 到低层 `view/` 破 6+ include 环
- [ ] `HY4` **— · S** — 删/接 ~1400 行死代码与假文档(`AsyncSystem::Future::then` 根本不存在;`PersistentCache`/`TileTextureCache` 死代码)

---

# Y 轴 · 平台能力(选赛道)

> 每一域都是一条独立赛道,内部有自己的 P0→Pn。选定方向后再拉 `.ref/` 深调研出细化路线图。

## D1 · 时间维度(4D)—— "活"的第一来源
现在整个引擎是静止快照。**基准**: Cesium 一等公民(Clock+Timeline);Google Earth Timelapse。**现状**: 零。
- [ ] `D1.1` **L** — 时间轴 / 时钟 / multiplier / 播放控制 UI
- [ ] `D1.2` **L** — 时变实体(CZML 式):位置样条插值、随时间出现/消失
- [ ] `D1.3` **M** — 历史影像时序切换(同点多年份)
- [ ] `D1.4` **XL** — 4D 时序瓦片集(施工进度、城市生长)

## D2 · 活数据层 —— "online" 的字面含义
**基准**: Cesium ion + 态势感知/数字孪生平台。**现状**: 无实时数据管道、无实体流式更新。
- [ ] `D2.1` **L** — 实体系统地基(billboard/label/model/polyline/point 流式增删改,承载所有活数据)
- [ ] `D2.2` **L** — 卫星轨道(TLE + SGP4 传播,在轨物体)
- [ ] `D2.3` **M** — 航班(ADS-B)/ 船舶(AIS)实时点馈送
- [ ] `D2.4` **L** — 实时天气(云图/降水雷达/风场粒子)、地震/火山/野火实时馈送
- [ ] `D2.5` **M** — IoT / GPS 实时位置(车队/无人机/设备)

## D3 · 分析 / 量算引擎 —— 从"看"到"算"
**基准**: ArcGIS Earth、Cesium Analytics。**现状**: 仅高度采样 + 拾取,无分析算子。
- [ ] `D3.1` **M** — 量算:测距/测面/测体积、地形剖面、坡度坡向
- [ ] `D3.2` **L** — 视域 / 通视分析(viewshed / line-of-sight,依赖 U1 深度/RTT)
- [ ] `D3.3` **M** — 日照 / 阴影分析(太阳位置 → 建筑投影,光伏/城规)
- [ ] `D3.4` **L** — 路由 / 导航 / 等时圈 / 地理围栏
- [ ] `D3.5` **M** — 缓冲区 / 叠加 / 空间查询(复用矢量系统 R-tree)
- [ ] `D3.6` **L** — 洪水淹没 / 水文 / 流域

## D4 · 真实感重建 —— 前沿中的前沿(2024-2026 最热)
**基准**: Cesium 支持 Google 3D Tiles + 正探索 3DGS。**现状**: 连 Draco/KTX2 都未解(见 CF5)。
- [ ] `D4.1` **M** — Google Photorealistic 3D Tiles 接入(全球实景,现成)—— 依赖 CF5
- [ ] `D4.2` **XL** — 3D Gaussian Splatting(3DGS)渲染:现实捕捉新范式,地理定位 splat 是当前热点 —— 依赖 U3
- [ ] `D4.3` **XL** — NeRF / 摄影测量 mesh 重建管线
- [ ] `D4.4` **L** — 倾斜摄影 / 现实点云场景融合

## D5 · 数据摄取 / 互操作(OGC 生态)—— 能吃多少世界
**现状**: 消费端 XYZ/WMS/WMTS/QM 有;生产端、点云、云原生格式全无。
- [ ] `D5.1` **L** — 云原生栅格:COG(Cloud-Optimized GeoTIFF)+ STAC 目录
- [ ] `D5.2` **L** — 点云:LAS/LAZ / COPC / 3D Tiles pnts 真实渲染(现 gl_PointSize 硬编 1.0)
- [ ] `D5.3` **M** — 矢量互操作:WFS / OGC API Features / GeoParquet / KML/KMZ / Shapefile
- [ ] `D5.4` **XL** — ⭐ **自建切片管线**:从源数据(DEM/倾斜摄影/BIM/点云)**生产** 3D Tiles/QM。**从"播放器"到"平台"的分水岭**
- [ ] `D5.5` **L** — 地形 / 影像自托管后端

## D6 · 渲染前沿(往前站,非追平)
多数依赖 U3(WebGPU/compute)。
- [ ] `D6.1` **XL** — 体积云 / 体积雾 / 体积光(天气可视化的灵魂)
- [ ] `D6.2` **L** — 真实海洋(FFT Gerstner 波 + 反射 + 海岸泡沫)—— 已有 sun-glint 是起点
- [ ] `D6.3` **XL** — WebGPU / Vulkan compute 后端(= U3,未来五年地基)
- [ ] `D6.4` **XL** — 实时全局光照 / 光追阴影(依赖 U1 + U3)
- [ ] `D6.5` **M** — 云层 / 极光 / 银河深化(已有 Rayleigh+Mie+Ozone 大气基座)

## D7 · 多天体 & 极致精度 —— 不止地球
**基准**: NASA、Cesium 支持多天体。**现状**: 单地球;深度精度战场进行中。
- [ ] `D7.1` **L** — 月球 / 火星 / 任意天体(任意椭球 + 各自数据集)
- [ ] `D7.2` **L** — 空间域:在轨卫星 / ISS / 碎片 / 锥形传感器覆盖 / 交会
- [ ] `D7.3` **XL** — 板块运动 / ITRF / 多历元参考框架(测绘级精度)
- [ ] `D7.4` **L** — RTC(relative-to-center)全双精度 ECEF(与深度破碎治理同战场)

## D8 · 协作 / 平台 / 沉浸
- [ ] `D8.1` **XL** — 多用户实时协作(共享视角 / 标注 / presence / 光标)
- [ ] `D8.2` **XL** — AR / VR / MR(头显数字地球、设备位姿 AR 叠加)
- [ ] `D8.3` **XL** — 云后端(场景持久化 / 权限 / 并发 / 离线内网部署)
- [ ] `D8.4` **L** — 跨端同源:Web(WebGL2/WebGPU)/ 原生 / 移动 / 头显

## D9 · 智能层(AI-native)—— 真正的差异化
2026 该有但少有引擎做深。
- [ ] `D9.1` **L** — 自然语言查询 → 空间查询求值("显示上海 30 层以上日照不足 2 小时的楼")
- [ ] `D9.2` **L** — 语义要素搜索 / 变化检测(两期影像 diff、违建识别)
- [ ] `D9.3` **M** — 地图 copilot(对话式导航 / 生成分析 / 解释数据)
- [ ] `D9.4` **XL** — 自动分类 / 场景理解(点云/影像自动分割建筑/植被/道路)

## D10 · 仿真 / 数字孪生
- [ ] `D10.1` **XL** — 洪水 / 火灾蔓延 / 烟羽 / 人群 / 交通仿真叠真实地形
- [ ] `D10.2` **XL** — 城市数字孪生(BIM/IFC 接入 + 室内导航 + 设备状态)
- [ ] `D10.3` **L** — 气候 / 环境模型网格可视化(参照 NVIDIA Earth-2)

---

# 进行中 / 已完成 / NO-GO(勿重复投入)

## 进行中
- [~] **可编辑矢量数据系统**:P0/P1a/P1b/P1c 已提交(FeatureStore + R-tree + 分桶脏区 + CDT fill + 线镶嵌)。**待推进**: P2 编辑核心/undo-redo、P3 贴地(stencil)、P4 MVT 底图、P5 SDF 标注、P6 stencil。设计见 `vector-data-system-design-2026-07-07.md`(§10 有 3 项待用户拍板)
- [~] **GPU 实例化批处理**:分支 `codex/surface-instancing-gpu-batch`;blend 悬崖已根治
- [ ] **无细数据区回落椭球**:设计成文,**未落地**。⚠️ 先验证触发条件——若跑 ion 全局数据集则为零改动 no-op(见设计 §0)

## 已完成(勿当 TODO)
- [x] 手势 A2 双指缩放惯性 / A5 指北针 / 球缘 spin3D 回落 —— 已提交(gesture audit 的这几项已过时)
- [x] 行星深度破碎:动态紧 near 平面根治(`9294f50de`)
- [x] blend 实例化悬崖:alpha-to-coverage(`038f00690`)
- [x] 相机 cartographic 记忆化 / 地形上传时间预算 / 太阳真实日地距离 + 大气门控 + rim light / 水面 sun-glint
- [x] selector 增量切面 L0-L2(捕获层已提交)
- [x] correctness F2 memo key + optional 高度 clamp 链

## NO-GO(已裁决,勿做)
- [!] **selector L3 增量剪枝**:真机实测对拖动**零杠杆**(`incremental-frontier-nogo`)
- [!] **float32 深度缓冲**:已试并回退,DEPTHDIAG 证 32F 无效;真因是 z_ndc 值域病态,已用 near 平面根治。⚠️ `planetary-depth-precision-float-depth-design` 文档 stale,把已生效的 near-plane 方案错标为 deprecated
- [!] **地平线卡顿"干净杠杆"**:无定论;fill 网格解耦只改观感不解卡(`overlay-terrain-coupling`)
- [!] **手势 A3 Android VelocityTracker / A4 缓动 flyTo** 仍 open,但 audit 里"球缘 spin3D""anchor-lock"已完成部分需从 TODO 剔除

---

# 怎么用这张表

```
         X 轴 · 引擎保真度(补债)──►  QW → CF → PF / CX / TS / HY
              │
   Y 轴 ──────┼──►  D1时间 D2活数据 D3分析 D4重建 D5摄取
  平台能力    │      D6渲染前沿 D7多天体 D8协作 D9AI D10仿真
 (选赛道)     ▼
```

- **X 轴打法**: 从 `QW1-7` 扫起(几乎纯接线,一两天出质变),`CX1` 优先(有崩溃风险),`U1/U2` 是解锁一大片的地基。
- **Y 轴打法**: **不全做**,先定"这颗地球要成为什么" → 选 1 条赛道:
  - 想要**真** → D4 真实感重建(先 CF5 → D4.1 Google 3D Tiles → D4.2 3DGS)
  - 想要**活** → D1 时间轴 + D2 活数据(先 D2.1 实体系统)
  - 想要**产品/变现** → D3 分析引擎 + D2 态势感知
  - 想要**平台地基** → D5.4 自建切片管线 + U3 WebGPU

**下一步**: 选定一条 Y 轴赛道,我拉 `.ref/` 对标成熟项目 + 出该赛道可执行路线图(带验证点)。十域技术选型差异极大,先定方向最省决策成本。
