# 视野区域瓦片加载速度 — 成熟地球引擎做法对比调研 + 落地建议

> 日期：2026-07-06 · 方法：拉六套参考引擎源码到 `.ref/` 本地逐行分析 + 规范/行业 web 调研 · 全部结论带 file:line 或 URL 证据
>
> 参考源（`.ref/`）：`cesium-native`、`cesium-js`（全 Core+Scene）、`cesium-js-foveated`、`tiles-renderer-js`（NASA-AMMOS 3DTilesRenderer）、`maplibre-gl-js`（v6.0.0-20）、`osgearth`（Rex）
>
> 被测基线：本引擎 `scaffold/src/earth_engine/`（Cesium-native 忠实移植，当前数据源 = 高德 raster 影像 + QM 地形，目标端 Android/GLES + macOS/Metal）

---

## 0. 执行摘要（TL;DR）

**结论一句话**：本引擎的**调度与优先级层已是生产级**（`(1−cosθ)·distance` 优先级、三组分级、per-frame 预算、cullRequestsWhileMoving 全部忠实移植 Cesium）。视野瓦片"加载快不快"的**观感天花板不在调度，而在三处**：① 下载完成→能画之间的 **GPU 上传/finalize 预算被卡得极紧**（默认每帧 1 个）；② LOD 切换是**硬切无过渡**（cross-fade 已建但默认关，无 geomorph）；③ 网络层**无 HTTP/2 多路复用**，高德那种"一屏几十张小 raster 瓦片"场景被 per-host 连接数与握手开销拖累。真正的"预测性预取"是**全行业（含 Cesium）都没做好的前沿**，收益高但浪费带宽风险也高，放最后。

### 落地优先级表（按"观感 ROI ÷ 工程量"排序，工时按 AI 协作基准估）

| # | 建议 | 层 | 观感收益 | 现状 | 主要风险 | 工时(AI) |
|---|------|----|---------|------|---------|---------|
| **P1** | 放宽 GPU 上传/finalize 每帧预算 + osgEarth 式"骨架瓦片不计预算" | 解码上传 | ★★★ 直接砍"下载完→显示"延迟 | 默认 finalize=1/rasterUpload=1，极紧 | 主线程上传卡顿；须 release 实测 | 0.5–1d |
| **P2** | 点亮已内建的 LOD cross-fade（`enableLodTransitionPeriod`）+ 评估 geomorph | 预测/观感 | ★★★ 消除 LOD 硬切 pop | cross-fade 已建默认关；无 geomorph | geomorph 在 TIN 上是大工程 | cross-fade 0.5d / geomorph 3–5d |
| **P3** | CurlMultiRequestScheduler 启用 HTTP/2 多路复用 | 网络缓存 | ★★★ 高德多小瓦片场景吞吐 | 无 HTTP/2、无 per-host 节流 | 服务器须支持；per-host 语义变化 | 0.5–1d |
| **P4** | 已建 `cullRequestsWhileMoving` 真机压测后默认开 + 校核 progressive 粗层先出 | 调度 | ★★ 运动期降载、首帧更快 | 已实现，默认关 | golden 字节变化（需重录） | 0.5d |
| **P5** | 视野外飞行/已发请求的**在途取消**（Cesium heap 驱逐语义） | 调度网络 | ★★ 快速平移时不等废请求 | 有 defer 无 cancel | 取消半程请求的重试成本 | 1–2d |
| **P6** | Draco / meshopt / KTX2 解码接入 | 解码上传 | ★★（解锁 ion/Google 3D Tiles） | KTX2/Basis 识别即拒，Draco/meshopt 全缺 | 第三方库集成体量、WASM/native | 2–4d |
| **P7** | 方向/速度感知**预测性预取**（低优先级 lane，配额封顶） | 预测 | ★★★（但风险高） | 完全缺失（Cesium 也没有） | 浪费带宽、缓存抖动、方向反转 | 3–5d |

> P1–P4 是"已有基础设施上拧螺丝"，性价比最高，应先做。P5–P7 是新机制，按需推进。
> **重要校准**：本引擎历史性能数据多数跑在 `-O0 debug` 上（见记忆 `perf-measured-on-debug-build`），膨胀 2–3×。以下所有"预算/耗时"类改动**必须在 release(-O2) 上评估**，否则会为 debug 假象过度优化。

---

## 1. 四层 × 六引擎 对比矩阵

图例：✅=有且完善 · 🟡=有但受限/默认关 · ❌=无

| 能力 | 本引擎 | Cesium(JS+native) | 3DTilesRenderer | MapLibre v6 | osgEarth Rex | 行业/规范 |
|------|:---:|:---:|:---:|:---:|:---:|:---|
| **① 调度与优先级** |
| 视锥优先级公式 | ✅ `(1−cosθ)·d` | ✅ 同+打包多段 | ✅ error/distance | ✅ 距离升序 | ✅ 距离/像素 | — |
| 高/中/低分组队列 | ✅ 3组 | ✅ 3队列+5ms/帧 | ✅ 3队列(dl25/parse5/node25) | 🟡 覆盖序即序 | 🟡 单优先级functor | — |
| 优先级容器 | 🟡 flat vector+每帧全排 | ✅ 定长堆+每帧重排+最低驱逐 | ✅ PriorityQueue | — | ✅ 动态functor调度时重读 | — |
| per-frame 加载预算 | ✅ 20发/20在途 | ✅ 50总/18每host | ✅ dl25/parse5 | 🟡 img16/帧8 | ✅ 并发4 | — |
| loadingDescendantLimit | ✅ 20 | ✅ 20 | 🟡 maxTilesProcessed250 | — | — | — |
| **② 预测/推测** |
| 中心优先(foveation) | ❌(地形no-op) | ✅ cone0.1/延迟0.2s | ❌ | ❌ | ❌ | 学术方向感知 |
| preloadAncestors/Siblings | ✅/✅ | ✅/✅ | ✅/✅ | ✅ retain机制 | 🟡 additive | — |
| 保粗父直到细子就绪 | 🟡 兜底覆盖 | ✅ | ✅ loadAncestors | ✅✅ retain子+父 | ✅✅ replace-refine | — |
| LOD cross-fade / geomorph | 🟡 fade已建默认关/无geomorph | 🟡 lodTransition | ❌ | ✅✅ 角色化cross-fade | ✅✅ geomorph | — |
| 相机速度/方向预取 | ❌ | 🟡 仅flight目的地 | ❌ | ❌(#116未做) | ❌ | ✅ 20%取→90%命中 |
| 飞行目的地预取 | ❌ | ✅ preloadFlightDestinations | — | — | — | — |
| 异步选择(离渲染线程) | ✅ shadow-tree(默认关) | ❌ | ❌ | ❌ | 🟡 job系统 | — |
| **③ 网络与缓存** |
| 最大并发 | ✅ curl_multi 20 | ✅ 50 | ✅ 25 | 🟡 16 | ✅ 4 | — |
| per-host 节流 | ❌ | ✅ 18/host+override | ❌ | ❌ | — | HTTP/2去分片 |
| HTTP/2 多路复用 | ❌ | 🟡 靠浏览器 | 🟡 靠浏览器 | 🟡 靠浏览器 | ❌ | ✅ 去6连接墙 |
| HTTP/3 QUIC | ❌ | 🟡 靠浏览器 | 🟡 | 🟡 | ❌ | ✅ 去传输HOL |
| 在途请求取消 | 🟡 defer无cancel | ✅ heap驱逐+cancel | ✅ AbortController | ✅ abortTile | ✅ revision失效 | — |
| 磁盘缓存 | ✅ 128MB内存+磁盘 | 🟡 靠浏览器HTTP缓存 | ❌ | 🟡 内存两级 | ✅ MemCache | PMTiles range |
| 内存LRU(字节预算) | ✅ 512MB | ✅ 512MB | ✅ 430MB自调度增量驱逐 | ✅ 视口推导 | ✅ 超时驻留 | — |
| 两级(视内+视外)缓存 | ❌ | ❌ | ❌ | ✅✅ in-view+out-view | 🟡 sentry | — |
| availability 批量元数据 | ✅ QM bitmask | ✅ layer.json+subtree | 🟡 | — | — | ✅ 3DTiles1.1 subtree |
| 退避重试 | ✅ 500ms→30s指数 | ❌(交客户端) | 🟡 | ✅ 过期重取 | ✅ 超时 | — |
| **④ 解码与上传** |
| worker 线程解码 | ✅ QM/glTF/纹理 | ✅ TaskProcessor | 🟡 主线程+three worker | ✅ vector/raster-dem | ✅ job池 | — |
| Draco 网格 | ❌ | ✅ | ✅(three) | — | — | 需for ion地形 |
| meshopt | ❌ | ✅ | ✅ | — | — | — |
| KTX2/Basis 纹理转码 | ❌ 识别即拒 | ✅ | ✅ | ❌ | ❌ | 需for Google |
| GPU 上传逐帧摊销 | 🟡 默认1/帧极紧 | ✅ processing队列 | ✅ 字节预算 | ✅ prepare/帧 | ✅✅ ICO异步预编译 | — |
| 独立窄 parse 队列 | ❌ | 🟡 | ✅✅ dl25/parse5 | ✅ worker | ✅ | — |
| GPU对象异步预编译 | ❌ | 🟡 | — | — | ✅✅ ICO 首绘不卡 | — |

---

## 2. 逐层深度分析

### 层① 加载调度与优先级

**成熟做法的共识**：三家 web 引擎 + osgEarth 全部收敛到"**单一可比较的优先级数 = 视轴夹角 × 距离**，配合高/中/低分组，每帧在预算内 drain"。差异在容器和动态性：

- **Cesium 的杀手锏是"定长优先级堆 + 每帧重排 + 最低优先级驱逐"**（`RequestScheduler.js:26` `priorityHeapLength=20`，`:305-311` 每帧 `requestHeap.resort()`，`:427-436` 堆满时弹掉并**取消**最低优先级请求）。相机一快平移，堆里过时请求自动沉底被驱逐，而不是傻等它们跑完。总并发 50、每 host 18（`RequestScheduler.js:57,65`）。
- **osgEarth 的动态性更强**：优先级是个**闭包，在调度时刻重新读取**（`PagedNode.cpp:233-236` `context.priority = [](){ return pnode->getPriority(); }`），入队后相机移近的瓦片会被重新提前。
- **3DTilesRenderer 的分离队列**：下载 25 / 解析 5 / 节点扩展 25（`TilesRendererBase.js:333-341`），解析队列故意窄，防解码突发饿死网络。

**本引擎现状**（`TileLoadPriorityPolicy` / `TilePriorityMetrics`）：
- 优先级公式 `(1−dot(dir,camDir))·distance` **逐字忠实 Cesium**（`TilePriorityMetrics.cpp:7-20`），多视图 min-reduce（`TileSelectionInputMetrics.cpp:76-82`）。三组 `Preload<Normal<Urgent`（`TileLoadPriorityPolicy.h:9-13`）。✅ 生产级。
- per-frame：`maximumSimultaneousTileLoads=20`、`maxNetworkRequestsPerFrame=20`（`FrameResourceBudget.h:27,32`），分 Terrain/Content/Raster lane。`loadingDescendantLimit=20`。✅
- **差距 1（容器）**：请求队列是 `std::vector` + 每帧 `std::sort` 全排 + O(n) 去重插入（`TileLoadQueue.cpp:9-28`、`TileLoadScheduler.h:51-52`），**不是**增量维护的堆。功能等价但 O(n log n)/帧；记忆记载此处曾是热点（已由 SchemeId interning 缓解）。**潜在问题**：可见瓦片数很大时（深缩放全屏）每帧全排有常数开销；非正确性问题。
- **差距 2（在途驱逐）**：本引擎有 `cullRequestsWhileMoving` 的**发送前 defer**，但没有 Cesium 那种"堆满时**取消**已入队/在途的最低优先级请求"。快速平移时，已发出但已划出视野的请求会**跑完才释放槽位**，而非立即取消让位给新可见瓦片 → 见 P5。
- **无 foveation**：`foveatedInterpolationCallback`/`foveatedTimeDelay` 完全缺失。**但这是对的**——Cesium 的 foveation 对 `replace && !skipLevelOfDetail` 的地形是 no-op（源码 guard），本引擎地形走 replace-refine，foveation 本就不生效。等价杠杆是 `cullRequestsWhileMoving`（见 P4）。
- **IncrementalFrontier**：Phase 0 只捕获不剪枝（`Tileset.h:98` `incrementalSelection=false`），每帧仍全量遍历。这是"选择耗时"而非"加载速度"问题，另有设计文档 `selector-incremental-frontier-design-2026-07-06.md`。

### 层② 预测性 / 推测加载

**这是全行业最不成熟的一层，也是差异最大的一层。**

- **真正的"相机速度/方向预取几乎无人做好"**：MapLibre 官方 issue #116 明说"naive 预取邻居不尊重相机移动方向，应外推地图未来会渲染到哪里"——**至今未实现**。Cesium 只有 `preloadFlightDestinations`（相机 flyTo 时预取**目的地**，`Cesium3DTileset.js:425`）。学术界有神经网络/Markov 预取器，"预取 20% 瓦片达 90% 命中率"（ScienceDirect S095741741300050X），但没进主流引擎。
- **主流引擎的"预测"其实是"保覆盖"**，三种形态：
  1. **保粗父直到细子就绪**：osgEarth `replace-refine` 最干净——`PagedNode.cpp:133-143`，子瓦片**加载且合并**前一直画父瓦片，绝不露洞。MapLibre 更进一步 `_retainLoadedChildren`（保已加载的子当替身）+ 向上找已加载祖先（`tile_manager.ts:362-396,648-681`）。
  2. **LOD 平滑过渡**：MapLibre **角色化 raster cross-fade**（`tile_manager_raster.ts`，parent 淡出/child 淡入，跨 5 级，含平移时边缘瓦片 self-fade）——**Cesium 没有，是 MapLibre 独有的观感杀手锏**。osgEarth `morphTerrain/morphImagery` 默认开（`TerrainOptions:48-49`）几何顶点在相邻 LOD 间 morph、影像 cross-fade，遮掉 LOD pop。
  3. **progressive 粗→细**：osgEarth `progressive` 先铺满粗层保证"永远有东西看"，再向内细化（`TerrainOptions:45,204`）。
- **Cesium foveation（中心先清晰）** 是 3D Tiles（photogrammetry/replace-refine 关）场景的观感杀手锏（`foveatedConeSize=0.1`、`foveatedTimeDelay=0.2s`），但如前述对本引擎地形无效。

**本引擎现状**：
- `preloadAncestors=true`/`preloadSiblings=true`（`Tileset.h:60-61`）——有，但只是**兜底覆盖**（culled sibling / refined ancestor 入 Preload 组），非渐进细化。
- **LOD cross-fade 已内建但默认关**：`enableLodTransitionPeriod=false`（`Tileset.h:80`，记忆 `ws2-lod-crossfade-prebuilt`）。整链已点亮 + SDK 接线 + 真机功能验证过，**只差默认开 + 像素级观感确认** → P2。
- **无 geomorph**（TIN 上做几何 morph 是大工程）。
- shadow-tree 异步选择（`asyncSelection=false` 默认关）是"把选择搬离渲染线程"，非推测加载。
- **无任何相机速度/方向预取** → P7（前沿，高风险）。

### 层③ 网络与缓存

**成熟做法共识**：
- **HTTP/2 多路复用是现代瓦片流的地基**：MapTiler 明确"HTTP/2 支持无限并发，domain-sharding 成了反模式（拆连接、破坏优先级）"。HTTP/2 修应用层 HOL 但仍走 TCP（一个丢包 stall 所有流）；HTTP/3 QUIC 让每流丢包独立 + 0-RTT，适合突发瓦片。
- **在途取消是平移/缩放响应性的关键**：Cesium heap 驱逐取消（`RequestScheduler.js:427-436`）、3DTilesRenderer `AbortController`（`TilesRendererBase.js:1585`）、MapLibre `abortTile`（`tile_manager.ts:209-214`）+ `cancelPendingTileRequestsWhileZooming`（缩放时丢掉飞掠层的请求）。
- **批量"什么瓦片存在"移出热路径**：3D Tiles 1.1 implicit tiling 的 **subtree availability bitstream**（Morton 序、常量位流可折叠）让一次 subtree 请求得知整块固定深度的存在性，免逐瓦片探测 404。PMTiles 单文件 + Range 请求，root 目录保证在前 16KiB，任意瓦片≤2 次可缓存目录请求。
- **两级缓存**：MapLibre `in-view` + `out-of-view LRU`（`tile_manager.ts:115-116`），视口推导大小（`≈视内瓦片×zoom级`），平移回来命中缓存零重取。

**本引擎现状**（`CurlMultiRequestScheduler`）：
- curl_multi **20 并发，保留 2 个 urgent 槽**（`CurlMultiRequestScheduler.h:15`、`.cpp:320`）。✅
- **无 HTTP/2**（未设 `CURLOPT_HTTP_VERSION`）、**无 keep-alive 显式配置**、**无 per-host 节流** → **P3 最大网络杠杆**。高德 raster 一屏几十张小瓦片，HTTP/1.1 下受 per-host 连接数 + 每请求握手拖累；HTTP/2 一条连接多路复用可显著提吞吐。**潜在问题**：须服务器支持 h2（高德 CDN 通常支持）；curl 需 nghttp2 编译；per-host 并发语义会变（h2 下"并发"是流不是连接）。
- **128MB 内存 + 磁盘 PersistentCache**（`HttpCache.h:40`，一 URL 一文件），zero-copy `shared_ptr<const>`。✅ 比纯浏览器方案强。
- 退避重试 500ms→×2→30s（`TileRetryBackoffPolicy.h`），项目自加、非 Cesium。✅
- QM availability bitmask（`TileAvailability.cpp:80-132`，位级索引 + Morton）。✅ 已是 subtree 思路。
- **差距**：① 无在途 cancel（见 P5）；② 无两级视内/视外缓存（MapLibre 式）——平移回头目前靠 512MB LRU 兜，通常够，非急件。

### 层④ 解码与上传

**成熟做法共识**：
- **解码全上 worker 线程**（Cesium TaskProcessor、MapLibre worker source）。Draco（网格）/ meshopt（顶点索引）/ KTX2-Basis（GPU 压缩纹理，免运行时解压直传）是标配。
- **GPU 上传逐帧摊销 + 异步预编译**：osgEarth 的 **ICO（IncrementalCompileOperation）** 是最干净的——merge 前在 worker/ICO 线程把 VBO/纹理**预编译好**，主线程只做便宜的 `addChild`，且**骨架/空瓦片不计每帧预算**（`PagedNode.cpp:501-506`），只有真几何被限速 → 首绘绝不 stall。3DTilesRenderer 的**分离窄 parse 队列**（parse 5）同理防突发。

**本引擎现状**：
- 解码**已在 worker 线程**：QM parse + TerrainGpuVertex（`QuantizedMeshTerrainProvider.cpp:2182`）、glTF + 纹理 RGB→RGBA（`GltfContentProvider.cpp:2400`、`GltfRenderResourcePreparer.cpp:42-84`）。✅
- **Draco / meshopt 完全缺失**（grep 零命中）；**KTX2 / Basis / DDS / ASTC 识别即拒**（`GltfModel.cpp:463-597`），只解 PNG/JPEG/WebP。→ P6：这**不是当前数据源的观感问题**（QM 非 Draco、高德是 JPEG），而是**解锁 Cesium ion World Terrain（Draco）/ Google Photorealistic 3D Tiles（KTX2）的前置**。记忆 `android-libcurl-no-ca-https-broken` 已落地 ion 支持接线，若要真用 ion 地形则 Draco 必需。
- **GPU 上传/finalize 每帧预算极紧** → **P1 最大观感杠杆**：`maxMainThreadFinalizesPerFrame=1`、`maxRasterUploadsPerFrame=1`（`FrameResourceBudget.h:35,37`），或按 `mainThreadTimeMs/2.5ms` 算（fallback 4 finalize/8 raster）。GPU buffer/texture **在主线程 GL 上下文创建**（`GltfRenderResourcePreparer.cpp:881-992`）。默认 1 个/帧意味着一屏 40 张瓦片下载完后要 **40 帧≈0.67s** 才全部上屏——这是"数据到了却慢慢冒出来"的直接元凶。**潜在问题**：放太宽会主线程 GL 上传卡帧（尤其大纹理）；须 release 实测找平衡点，或走 osgEarth 式骨架豁免 + 大纹理限速。

---

## 3. 落地建议详述（每条含潜在问题）

> 排序 = 观感 ROI ÷ 工程量。P1–P4 建议优先，都是"已有基础设施拧螺丝"。

### P1 — 放宽 GPU 上传/finalize 预算 + 骨架瓦片豁免 ★最高性价比
**做什么**：把 `maxMainThreadFinalizesPerFrame` / `maxRasterUploadsPerFrame` 从 1 提到自适应值（按 release 实测的主线程时间预算算），并借鉴 osgEarth：**便宜的骨架/小瓦片不计每帧配额，只对大纹理/重几何限速**（`PagedNode.cpp:501-506` 思路）。
**为什么**：这是"下载完成→像素上屏"的直接闸门，默认 1/帧把一屏瓦片的显示摊到几十帧。
**潜在问题**：① 主线程 GL 上传是同步的，放太宽会掉帧——须在 **release(-O2)** 上按机型实测（记忆 `perf-measured-on-debug-build`：debug 数据不可用）；② Metal 与 GLES 上传成本不同，可能要分平台预算；③ 大纹理（高德瓦片 256²/512²）单个就可能超预算，需按字节而非按个数限速。
**验证**：一屏 40 瓦片场景，测"selection 完成→最后一张上屏"的帧数，目标从 ~40 帧降到 <10 帧且不掉出 60fps。

### P2 — 点亮 LOD cross-fade + 评估 geomorph ★观感
**做什么**：`enableLodTransitionPeriod=true` 默认开（已内建、已真机功能验证，记忆 `ws2-lod-crossfade-prebuilt`），做像素级 ghosting 主观确认。进一步评估 geomorph（几何顶点在 LOD 间 morph）。
**为什么**：MapLibre 角色化 cross-fade + osgEarth geomorph 是两家的观感杀手锏，消除 LOD 硬切 pop。本引擎 cross-fade 已建好只差开。
**潜在问题**：① cross-fade 期间父子瓦片同时驻留 → 短时显存 + draw call 翻倍（`kickDescendantsWhileFadingIn=true` 已缓解）；② geomorph 在 QM 的 TIN（非规则网格）上做顶点 morph 是**大工程**（cesium 在 TIN 上也偏离），除非 ghosting 碍眼否则 cross-fade 足够；③ 半透明混合顺序在地形自遮挡处可能有瑕疵。
**验证**：录制 LOD 切换瞬间对比开/关，pop 消失且无鬼影残留。

### P3 — CurlMultiRequestScheduler 启用 HTTP/2 多路复用 ★网络吞吐
**做什么**：`CURLOPT_HTTP_VERSION = CURL_HTTP_VERSION_2TLS` + `CURLOPT_PIPEWAIT=1`（让 curl 等一条连接复用而非开多条），确认 vcpkg curl 带 nghttp2。
**为什么**：高德 raster 一屏几十张小瓦片是典型"多小请求同域"场景，HTTP/2 单连接多路复用 + 头压缩直接提吞吐、去 per-host 连接墙。
**潜在问题**：① 须服务器支持 h2（高德 CDN 通常支持，须实测确认；QM 地形服务器未必）；② curl 编译须含 nghttp2（记忆 `android-libcurl-no-ca-https-broken` 提示 Android 用 vcpkg OpenSSL，须确认 h2）；③ 当前"20 并发 + 2 urgent 槽"的语义在 h2 下变成"流"而非"连接"，节流逻辑需重新校核（可能反而该放宽并发）；④ 混合 h1/h2 源要能优雅回落。
**验证**：抓包确认单连接多流；一屏瓦片全部到达的墙钟时间对比。

### P4 — cullRequestsWhileMoving 真机压测后默认开 ★运动降载
**做什么**：`cullRequestsWhileMoving=true`（已实现，`TileMotionCullPolicy.h`），真机验证运动期降载效果，校核 progressive 粗层是否先出保证"运动中也有东西看"。
**为什么**：这是对 replace-refine 地形**唯一真正有效的运动降载杠杆**（foveated 无效）。运动期跳过"回来时多半已划走"的瓦片请求，把带宽让给停下后真正要看的。
**潜在问题**：① 默认开会改变 golden（选择/加载序变化），需重录基线（记忆载 golden 对拍机制）；② multiplier=60 是 cesium-js 默认，本引擎相机单位/惯性可能需重调阈值；③ 运动中过度 cull 会让"边移边看"变糊，须与 P2 progressive 配合。
**验证**：真机快速平移，测运动期发出的请求数下降、停下后收敛时间不变差。

### P5 — 视野外/已发请求的在途取消
**做什么**：补齐 Cesium heap 驱逐语义——请求队列满或瓦片划出视野时，**主动 cancel** 已入队/在途的最低优先级请求（curl `curl_multi_remove_handle`），而非等它跑完。
**为什么**：快速平移时已发出但已无用的请求占着 20 个并发槽，新可见瓦片被饿。Cesium/3DTilesRenderer/MapLibre 全都 cancel。
**潜在问题**：① 取消半程请求，若相机回摆需重发 → 退避策略须避免抖动重取；② curl multi 移除 handle 的连接复用影响（h2 下取消单流成本低，与 P3 协同）；③ 需区分"划出视野"vs"临时被遮挡"，避免误杀马上要回来的。
**验证**：快速来回平移，测并发槽被废请求占用的时长下降。

### P6 — Draco / meshopt / KTX2 解码接入
**做什么**：集成 Draco（网格）、meshopt、KTX2+Basis 转码（`GltfModel.cpp:463-597` 现在识别即拒的分支改为真解码），解码放 worker。
**为什么**：**解锁数据源**——Cesium ion World Terrain 用 Draco，Google Photorealistic 3D Tiles 用 KTX2。当前高德+QM 不需要，故非观感急件，但若走 ion/Google 路线则**硬前置**。
**潜在问题**：① 第三方库体量（draco/basisu 的 native/WASM），Android APK 增重；② KTX2 转码目标格式随 GPU 而变（Metal ASTC/GLES ETC2），须查询设备支持；③ 与现有 worker 池的线程预算竞争。
**验证**：加载一个 Draco 地形瓦片 / KTX2 纹理 glTF，正确出图、解码在 worker、无主线程卡顿。

### P7 — 方向/速度感知预测性预取（前沿，高风险）
**做什么**：从相机速度/方向外推"未来几帧会看到哪"，把那些瓦片以**低优先级 lane + 配额封顶（如总带宽 20%）**预取，方向反转立即 cull。
**为什么**：这是"移动到哪、瓦片已在那"的观感终极形态（学术界 20% 取→90% 命中）。**但全行业主流引擎都没做好**（含 Cesium、MapLibre），说明工程/收益比微妙。
**潜在问题**：① 猜错就是纯浪费带宽 + 缓存抖动，移动端流量敏感；② 必须严格低于视内瓦片优先级（P1 的 lane 隔离是前提），否则饿死可见帧；③ 方向频繁变（拖拽抖动）会预取振荡；④ 与 P4 cullWhileMoving 语义可能冲突（一个要省、一个要抢），需统一运动策略。**建议**：留到 P1–P5 落地、运动策略稳定后再做，作为可开关实验特性。

---

## 4. 我可能理解偏的地方（请你校准）

按全局规则"有歧义给多解读"，这次调研有两处我做了判断，你可能想调整方向：

1. **我把"加载速度"重心放在"下载完→上屏"和"观感 pop"，而非"纯网络下载更快"**——因为从证据看，本引擎网络层（20 并发+磁盘缓存+退避）已不弱，真正卡观感的是 P1 上传闸门和 P2 硬切。如果你实测发现瓶颈就是**网络下载本身慢**（弱网/服务器慢），那 P3 HTTP/2 + P5 取消应提到最前。
2. **我把 P6（Draco/KTX2）判为"解锁新数据源"而非"当前观感急件"**——前提是你继续用高德+QM。若你近期就要切 Cesium ion / Google 3D Tiles，P6 立刻升为 P1 级前置。

---

## 5. 参考源码索引（`.ref/`）

| 项目 | 路径 | 关键文件 |
|------|------|---------|
| cesium-native | `.ref/cesium-native/` | `Cesium3DTilesSelection/`（TilesetContentManager、TilesetOptions.h） |
| CesiumJS | `.ref/cesium-js/packages/engine/Source/` | `Core/RequestScheduler.js`、`Scene/Cesium3DTile.js`（updatePriority）、`Scene/QuadtreePrimitive.js`、`Scene/Cesium3DTilesetTraversal.js`（foveation） |
| 3DTilesRenderer | `.ref/tiles-renderer-js/src/` | `core/renderer/utilities/{PriorityQueue,Scheduler,LRUCache}.js`、`core/renderer/tiles/TilesRendererBase.js` |
| MapLibre v6 | `.ref/maplibre-gl-js/src/` | `tile/tile_manager.ts`、`tile/tile_manager_raster.ts`（角色化cross-fade）、`source/*_worker_source.ts`、`geo/covering_tiles.ts` |
| osgEarth Rex | `.ref/osgearth/src/osgEarth/` | `PagedNode.cpp`（PagedNode2/PagingManager/merge）、`GLUtils.cpp`（ICO）、`SimplePager.cpp`、`TerrainOptions`（morph/progressive/concurrency） |

**Web 关键源**：Google [Map Tiles Policies](https://developers.google.com/maps/documentation/tile/policies)（禁持久缓存）、[create-renderer](https://developers.google.com/maps/documentation/tile/create-renderer)（session key）、[3D Tiles ImplicitTiling spec](https://github.com/CesiumGS/3d-tiles/blob/main/specification/ImplicitTiling/README.adoc)（subtree availability）、[MapTiler HTTP/2 vs sharding](https://docs.maptiler.com/guides/maps-apis/maps-platform/http2-in-maptiler-cloud-versus-domain-sharding-technique/)、[PMTiles v3 spec](https://github.com/protomaps/PMTiles/blob/main/spec/v3/spec.md)、[MapLibre #116 预取](https://github.com/maplibre/maplibre-gl-js/issues/116)、[NN 瓦片预取 20%→90%](https://www.sciencedirect.com/science/article/abs/pii/S095741741300050X)。
