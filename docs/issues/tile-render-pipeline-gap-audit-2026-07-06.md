# 瓦片渲染链路深度调研 — 与 cesium-native 的差距审计

- **日期**: 2026-07-06
- **分支 / commit**: `codex/surface-instancing-gpu-batch` @ `7d84202ac`
- **对标基准**: cesium-native（本引擎明确的移植血统）；观感天花板参照 Google Earth
- **方法**: 7 路并行深调（选择/流式/地形/影像/渲染/功能/架构），逐子系统对齐源码后判定差距。评级 P0(阻断)/P1(重要)/P2(应修)/P3(可延后)
- **范围**: `scaffold/src/earth_engine/`（499 文件）

> ⚠️ **本报告纠正 AI_INDEX 的一处重大低估**：AI_INDEX 只描述了地形 glTF 路径，但 `content/GltfContentProvider.cpp`(4267 行) 实为**完整的多格式 3D Tiles 加载器**——b3dm/i3dm/pnts/cmpt/glb 都有真实的按魔数分派解码器，tileset.json（含外部/嵌套）可解析，glTF 动画/GPU 实例化已接线。功能差距集中在"高级生产层"（压缩编解码、样式语言、feature 拾取、implicit tiling），而非基础格式。

---

## 0. 总体结论

**这是一个高质量的 cesium-native 移植，算法保真度是它最强的资产，观感与生产级功能是最大的短板。**

- **算法保真（选择/流式/地形/影像映射）** = 生产级。SSE 公式、kick、遮挡、fog、forbidHoles、逐顶点重投影、多影像 alpha 合成、字节预算 LRU、in-flight 合并——逐一核实为忠实移植，且有 170 个测试文件 + cesium golden 逐帧字节对拍护航。
- **观感（shading ceiling）** = 被四根缺失支柱封顶：**(a) 没有真正的 PBR BRDF**（Blinn-Phong 近似）、**(b) 没有任何 ambient/IBL**、**(c) 地表没有 ground atmosphere/fog**、**(d) 没有任何后处理**（AA/HDR/阴影），后三者全部卡在 `createFramebuffer` 是 stub 上。
- **功能完整度** = 基础 3D Tiles 齐全，但缺 **Draco/meshopt/KTX2 压缩解码**（真实生产瓦片大量使用，直接导致加载失败/无纹理）、**3D Tiles 样式语言**、**per-feature 拾取**、**implicit tiling**。
- **架构/工程** = 分解设计净正向（seam 有独立单测），但 **07-05 审计的每一个 P1 仍然 OPEN**（工作树只动了 TileBoundsMetrics.cpp）。

**四个"高杠杆低成本"速赢**（详见 §9）：把已算好的 `SkyGradient::ambientColor()` 接进 ambient uniform；swapchain 层开 MSAA；把 water-mask 的 no-op mix 换成真实水面着色；默认打开已内建的 LOD cross-fade。

---

## 1. 优先级矩阵（跨子系统 Top 项）

| # | 差距 | 维度 | 子系统 | 严重度 | 成本(AI协作) |
|---|------|------|--------|--------|------|
| 1 | 无真正 PBR BRDF（Blinn-Phong 近似） | 观感 | 渲染 | **P1** | 中 |
| 2 | 无 ambient/IBL；`ambientColor()` 算了但没接线 | 观感 | 渲染 | **P1** | **低(速赢)** |
| 3 | 地表无 ground atmosphere/aerial perspective/fog | 观感 | 渲染 | **P1** | 中 |
| 4 | 无任何 AA（MSAA/FXAA/TAA） | 观感 | 渲染 | **P1** | **低(速赢)** |
| 5 | 缺 Draco + meshopt + KTX2/Basis 解码（真实瓦片大量用） | 功能 | 内容 | **P1** | 中-高 |
| 6 | Water mask 全流程解码上传却是 `mix(base,base,water)` no-op | 观感 | 地形/渲染 | **P1** | **低(速赢)** |
| 7 | HTTP 响应永不校验（无 ETag/Cache-Control/304/过期） | 保真 | 流式 | **P1** | 中 |
| 8 | 影像 provider 完全绕过 HTTP/磁盘缓存 | 保真/性能 | 流式 | **P1** | 中 |
| 9 | `FrameResourcePriority` 在预算里被静默忽略 | 保真/性能 | 流式 | **P1** | 低 |
| 10 | 无退避重试；`FailedTemporarily` 每帧重打服务器（自造 DoS） | 韧性 | 流式 | **P1** | 低 |
| 11 | 3D Tiles 样式语言缺失（无法按属性 color/show/hide） | 功能 | 内容 | **P1** | 中 |
| 12 | per-feature 拾取缺失（batch/feature table 解析了但没暴露） | 功能 | 交互 | **P1** | 低-中 |
| 13 | WMS crs/BBOX 轴序硬编码 1.3.0，version 却可配 → 1.1.1 请求畸形 | 保真 | 影像 | **P1** | 低 |
| 14 | 07-05 审计的所有 P1 correctness 项仍 OPEN（2 泄漏+1 竞争+3 递归+2 拷贝+1 遍历） | 架构/并发 | 全局 | **P1** | 中 |
| 15 | GoogleMapTiles availability 向量无锁跨线程读写（UAF 风险） | 并发 | 影像 | **P1** | 低 |

> 注：无 P0（阻断级）。这是一个能跑、能出正确帧的引擎；差距是"离成熟"而非"离能用"。

---

## 2. 瓦片选择 / LOD 遍历

**状态：忠实、良好分解，差距窄。** `visitTile`/kick/遮挡/SSE/fog/forbidHoles 与 cesium-native 行为逐行一致（S1 golden 逐帧字节相等佐证）。

| 差距 | 维度 | 严重度 | 证据 | vs Cesium | 置信 |
|------|------|--------|------|-----------|------|
| 无 `ITileExcluder` 支持 | 功能 | P2 | 全树 grep `Excluder`/`shouldExclude` = 0 hit | cesium 在 frustum/fog 前跑 excluder pass（clipping polygon/region mask 强制剔除） | 高 |
| 增量 frontier 只捕获不剪枝（未建） | 性能/功能 | P2 | `TileIncrementalFrontier.h:27`；设计见 `selector-incremental-frontier-design-2026-07-06.md` | 非 cesium 差距（cesium 也全遍历）；本项目自定的唯一数量级杠杆，未建 | 高 |
| 等价 oracle 按无序 multiset 比对 render set | 架构(验证基座) | P2 | `TileSelectionEquivalence.h:54-73` 排序后比 | additive refine 与半透明合成 render 顺序有语义；oracle 会漏掉增量构建的顺序置换 | 中 |
| 子瓦片未按近到远排序 | 保真/性能 | P3 | `TileSelectionChildTraversal.h:18` 按 vector 序 | **无分歧**——cesium 同处也是 TODO，两边都按出现序 | 高 |
| replace-refine 后代扫描用当前帧 previous-state 而非快照 | 保真 | P3 | `TileSelectionTraversalDetailsBuilder.cpp:16` vs cesium `forEachPreviousDescendant` | 稳定树等价；子树 detach 后一帧可能误算 → 多余 kick 或一帧洞 | 低(建议做定向 golden 测试) |
| `viewerRequestVolume` 门控在本 visit 路径有、cesium inline selection 无 | 保真 | P3 | `TileSelectionVisitPreparation.cpp:46-49` | 是**新增**而非缺失；带 request_volume 的瓦片集输出会偏离 cesium | 中 |
| shadow(异步)选择每帧全量重建影子树 | 性能/架构 | P3 | `TileSelectionShadowRunner.cpp:63` `shadowTree_.build()` 无条件 | cesium 无影子树（同步选择）无基准；全量重建是 worker 上 O(live) 分配抖动 | 中 |

**已核实忠实（勿重查）**: SSE 公式、逐 volume-kind 精确距离、fog 表、kick + loadingDescendantLimit + 队列 checkpoint、遮挡 children-union、`RenderedAndKicked`/`RefinedAndKicked` 非对称、additive 父+子、多视图、preloadAncestors/Siblings 优先级组。

---

## 3. 内容加载 / 流式 / 缓存 / GPU 上传

**状态：LRU 卸载、优先级排序加载队列、in-flight 合并、线程模型均忠实且稳。** 差距集中在 **HTTP 缓存保真**与**失败/优先级处理**。

> **纠正一条 stale memory**：`AsyncSystem::Future::then` 在本树**并不存在**（类里只是个 `std::future` 包装，doc-comment 描述了从未实现的 `.then` 链——是"假文档抽象"，不是泄漏）；`AsyncSystem::run` 走**有界线程池**。此前"每 `.then` 泄漏 detached thread"的判断不成立。

| 差距 | 维度 | 严重度 | 证据 | vs Cesium | 置信 |
|------|------|--------|------|-----------|------|
| HTTP 响应永不校验（无 ETag/Cache-Control/304/过期） | 保真/功能 | **P1** | `PlatformBridge.h:73-78` 回调只有 `(status, body)`——响应头被结构性丢弃；`HttpCache::put` 从不设 `expiryTime` | cesium `CachingAssetAccessor` 解析 Cache-Control/Expires/ETag，stale 时发条件 GET，304 刷新。此处每个瓦片**永久缓存**，永不能重验 | 高 |
| 影像 provider 完全绕过 HTTP/磁盘缓存 | 保真/性能 | **P1** | `HttpCache` 仅被 QM/heightmap 引用；`XYZ`/Bing/Google/TMS/WMS/WMTS 全直连 `platformBridge_->get` | cesium 所有 asset 走单一 CachingAssetAccessor（内存+SQLite）。此处影像 LRU 逐出后回摇即全量重下+重解码，重启不留存 | 高 |
| `FrameResourcePriority` 在预算里被静默忽略 | 保真/性能 | **P1** | `FrameResourceBudget.cpp:22-24,94-96` priority 形参无名/未用；Preload/Normal/Urgent 共用一个计数器 | cesium 让 urgent 可预留/抢占预算。此处 `maxMainThreadFinalizesPerFrame=1` 下 urgent 可能被 preload 饿死 | 高 |
| GPU 上传队列是 FIFO 非优先级序 | 性能/保真 | P2 | `GpuUploadQueue.h:32-38` `front()`；doc-comment"pop 最高优先"是假的 | cesium 按瓦片优先级在主线程时间预算内 drain，高 SSE 先上屏 | 高 |
| 无退避重试；`FailedTemporarily` 每帧重打；无 429/503/Retry-After | 功能/架构 | **P1** | `TileLoadRequestPlanner.cpp:9,19` 下一帧立即再合格；provider 只判 `!=200`；头被丢无法读 Retry-After | 限流/抖动源被帧率级重打 = 自造 DoS/thrash | 高 |
| 无 per-host 请求节流 | 功能 | P2 | `CurlMultiRequestScheduler` 只有全局 20 上限，无 host 分桶 | cesium 按 host 限并发，慢源不能独占连接池饿死另一 overlay/terrain | 中 |
| `SharedAssetDepot` 字节预算浅 `sizeof` 低估 | 保真 | P2 | `SharedAssetDepot.h:183-185` `assetSize`=`sizeof(TAsset)` | 16MiB 预算实测的是控制块大小，逐出几乎不触发 | 高 |
| `PersistentCache`: `std::hash` 文件名(碰撞)+ `ensureDir` no-op | 架构/正确性 | P2 | `.h:60-64` `std::hash(url)`；`.h:67-73` 空 call_once | cesium 用 SHA + SQLite 按全 URL 键。此处两 URL 可静默碰撞，目录不存在时 save 静默失败 | 高 |
| 磁盘持久 fire-and-forget，无回读/无错误上抛 | 架构 | P3 | `HttpCache::persistAsync` 忽略结果；`getResponse` miss 不回落磁盘 | cesium 内存 miss 透明落磁盘层 | 中 |

---

## 4. 地形

**状态：QM 解析（头/zigzag/edge/oct 法线 ext1/watermask ext2/metadata ext4）完整且有界；skirt 字节忠实；glTF-terrain 上采样是真实填洞路径（逐子窗口重归一化 UV + 重建 skirt + 缩放 watermask）。**

| 差距 | 维度 | 严重度 | 证据 | vs Cesium | 置信 |
|------|------|--------|------|-----------|------|
| Water mask 解码上传却从不渲染（死水面） | 保真/功能 | **P1** | `Renderer.cpp:960,1013` `applyGltfWaterMask` 收在 `mix(base,base,water)` = no-op | cesium 用 mask 混水色+动画法线扰动高光+fresnel。此处整条 QM 水流水线零视觉差异 | 高 |
| 地形光照是半球假阴影非逐顶点法线 PBR | 保真 | P2 | `Renderer.cpp:1021` `shade=mix(0.72,1.0,smoothstep(NdotL))`；无 ambient/specular | oct 法线**已解析上传**却只喂这一个 clamp lobe；缺法线时回落面平均法线(cesium 不这么做→低顶点瓦片显刻面) | 高 |
| 无 LOD geomorph（层级几何形变） | 保真 | P2 | 地形 vertex shader `Renderer.cpp:867-886` 无 morph uniform | cesium 随 SSE 逼近阈值把顶点高度朝父面形变消 pop。此处会 pop | 高 |
| HeightmapTerrainProvider 是 Mapbox/Terrarium-RGB 解码器非 cesium HeightmapTerrainData | 功能/架构 | P2 | `HeightmapTerrainProvider.cpp:267-344` 只解出高度数组，不建 TIN/不进 glTF 绘制 | cesium 建规则网格+skirt+法线+可 upsample。此处 heightmap 实为高度查询源，与 QM 渲染路径分叉 | 高 |
| QM 请求缺 `Accept: application/vnd.quantized-mesh;extensions=…` 头 | 保真/功能 | P2 | extensions 只作 query 参数；无 Accept 头，无 content-type 校验 | cesium 用 Accept 内容协商 normals/watermask/metadata。走 Accept 协商的服务器(ion 等)会静默降级为无扩展 | 中 |
| layer.json `bounds` 不解析；无 `{s}` 子域 | 功能 | P3 | 只从 scheme+tilekey 算 bounds；模板无 `{s}` | cesium 用 bounds 跳过覆盖外请求(避免区域数据集 404 风暴——正对应已知部分覆盖 skirt 墙问题) | 中 |
| 上采样 skirt 法线用纯 geodetic 覆盖插值法线，clip 缝法线连续性近似 | 保真 | P3 | `GltfTerrainUpsampler.cpp:436-477` | cesium 让编码法线一致穿过插值，上采样与原生瓦片着色一致。仅掠射光下可见 | 低 |

**已核实忠实（勿重查）**: QM 二进制解析（含非规范 4 字节对齐 16bit 索引探测）、edge 索引边界校验、oct 法线解码+上传(32B TerrainGpuVertex)、skirt 条带绕序、glTF-terrain 上采样填洞、availability quadtree+运行时 metadata 合并、WebMercator 地形 texcoord 重写。

---

## 5. 影像叠加（Raster Overlay）

**状态：渲染侧（逐顶点重投影/多投影 texcoord/多叠加 alpha 合成/源瓦片 combine/LOD 选择/UV 重归一化）是忠实扎实的 cesium 移植。** 差距集中在**协议广度**与**呈现打磨**。

| 差距 | 维度 | 严重度 | 证据 | vs Cesium | 置信 |
|------|------|--------|------|-----------|------|
| WMS crs/BBOX 轴序硬编码 1.3.0，version 却可配 | 保真 | **P1** | `WebMapServiceImageryProvider.cpp:311` 恒 `crs=EPSG:4326`；`:319-322` BBOX `south,west,north,east` | cesium 按 version 切换：1.1.1 用 `srs=`+lon/lat 序，1.3.0 用 `crs=`+按 CRS 轴序。请求 1.1.1 会畸形 | 高 |
| CreditSystem 存在但未接线，归属是平铺字符串拼接 | 架构/功能 | P2 | `layers/CreditSystem.{h,cpp}` 全树无外部引用；Bing 解析了 per-credit 覆盖区却丢进平铺串 | cesium 把 credit 喂 CreditSystem 出逐帧快照+按覆盖区门控。此处覆盖感知归属丢失（法律/署名） | 高 |
| WMS/WMTS 无 GetCapabilities 驱动配置 | 功能 | P2 | WMS 只校验 capabilities 不提取；WMTS 无 capabilities 解析 | cesium 可解析 GetCapabilities 自动填 TileMatrixSet/formats/bounds | 高 |
| 无叠加色彩调整参数(gamma/亮度/对比/饱和/色相) | 功能 | P2 | `RasterOverlay.h:42-72` 只有 opacity/visible/msse；shader 只 `alphaOver` | 只 honor opacity | 高 |
| Max 4 mapped overlay，静默截断 | 架构/保真 | P2 | `RenderCommand.h:17` `kMaxGltfRasterOverlays=4`；`GltfDrawCommandBuilder.cpp:289-291` 静默 break | cesium 合成任意 N。第 5+ 叠加在绘制时静默丢弃 | 高 |
| WMS 瓦片化硬编码 180°地理 quadtree，忽略 overlay 的 TileScheme | 保真 | P2 | `.cpp:301-309` 从 ±90/±180 固定网格算 | cesium 从 overlay 投影/瓦片方案推请求矩形(支持 WebMercator WMS) | 中 |
| mapped-raster 纹理上传强制关 mipmap | 性能/保真 | P2 | `RasterOverlayTileProvider.cpp:3574-3579` 硬编码 `generateMipmaps=false` | cesium 生成 overlay mipmap。关掉则缩小影像 aliasing，`maxAnisotropy=4` 无 mip 链帮不上 | 中(移动端成本权衡或有意，待决策) |
| Bing 无 imagerySet 选择(Aerial/Road/…) | 功能 | P2 | `BingMapsImageryProvider.cpp:349-362` 单一 metadata URL | cesium `BingMapsStyle` 选端点 | 高 |
| 缺 cesium 有的叠加类型(Ion/SingleTile/UrlTemplate 通用) | 功能 | P2/P3 | 现有 XYZ/TMS/WMS/WMTS/Bing/Google/Debug | 最有价值缺口：SingleTile(静态图/logo，廉价)、ion imagery | 高 |
| 无 cutout/裁剪矩形 | 功能 | P3 | grep `cutout` = 0 | cesium `RasterOverlayCutoutCollection` 打洞 | 高 |

**已核实忠实（勿重查）**: 逐顶点 WebMercator 重投影(精确 atanh)、多投影共存、多叠加 alpha 合成、源瓦片 combine、target-screen-pixels LOD、TMS tilemapresource.xml 解析。

---

## 6. 渲染质量 / 观感

**状态：材质*管线*广而忠实；*光照/BRDF/大气*模型是风格化近似；完全没有后处理基础设施（全卡在 `createFramebuffer` 是 stub）。**

| 差距 | 维度 | 严重度 | 证据 | vs 成熟引擎 | 置信 |
|------|------|--------|------|------------|------|
| 无真正 PBR BRDF（Blinn-Phong 近似 + PBR 输入） | 观感 | **P1** | `Renderer.cpp:731-777`(GLSL)+MSL 镜像；`diffuse=smoothstep(NdotL)`、无微面分布/几何项/基层 Fresnel | cesium 用真 Cook-Torrance(GGX+Smith+Schlick)。金属/粗糙介质会显平/塑料，不随视角变 | 高 |
| 无 ambient/IBL/环境光——全场景单方向光 | 观感 | **P1** | 无 `u_ambient`/irradiance/BRDF-LUT；`SkyGradient::ambientColor()` 算了但**零处接线** | cesium/GE 有 sky irradiance；此处 glTF 背光面近黑，金属零反射。建筑阴面死黑 | 高 |
| 地表无光照模型——平铺影像 × 标量 | 观感 | **P1** | `Renderer.cpp:1021-1022` `shade=mix(0.72,1.0,smoothstep(NdotL)); color=base*shade` | cesium 地表逐片元太阳光照+ground atmosphere+fog。此处地形像平铺贴图非受光 3D 面 | 高 |
| 地表无 ground atmosphere/aerial perspective/fog | 观感/功能 | **P1** | `AtmosphereBackgroundPass.cpp:123-125` **discard 所有朝地射线**("交给地表 shader")，但地表 shader 无大气/fog 代码；`u_fogColor` 只在**已废弃死** shader 里 | vs Cesium/GE 最大观感差距之一。天空在地平线戛然而止，远地形保持全对比——"球上贴平图"感 | 高 |
| 无任何 AA(MSAA/FXAA/TAA) | 观感 | **P1** | `platform/` 无 sampleCount/multisample；`createFramebuffer` 两后端都 **stub 返回 nullptr** | cesium 用 FXAA(+MSAA)，GE 重度 AA。skirt/瓦片边/海岸线/建筑轮廓会爬行闪烁，高 DPI 移动端尤甚 | 高 |
| 无 HDR/tone-mapping/bloom——纯 LDR | 观感/功能 | P2 | 无 tonemap/HDR/ACES 引用；直写 8bit 默认帧缓冲 | cesium 支持 HDR+ACES+bloom。此处高光硬裁白，emissive/太阳无法 bloom 过几何 | 高 |
| 无阴影(shadow map/自阴影/CSM) | 功能 | P2 | 无 shadow-map/depth-pass/CSM 代码(无帧缓冲可渲深度) | cesium 有 CSM，GE 有地形自阴影(日出日落山影)。此处山不投影建筑不遮地 | 高 |
| 水面渲染是字面 no-op（解析了从不着色） | 功能/观感 | P2 | 见 §4 water-mask 项；`mix(base,base,water)` | cesium 动画水面+高光；GE 反射海洋。此处海洋只是平铺影像 | 高 |
| LOD cross-fade 内建但默认关；无 geomorph | 功能 | P2 | `enableLodTransitionPeriod=false`(`TileLodTransitionFrameUpdater.h:15`) | GE/cesium 用 geomorph 隐藏 LOD 交换。此处 cross-fade 关时 pop，开也只淡 alpha 不形变轮廓 | 高 |
| GLES 高级材质 sampler 别名静默禁用扩展*纹理* | 功能 | P3 | `Renderer.cpp:213-222` GLES 上 anisotropy/specular/clearcoat/sheen/transmission 纹理 `#define` 到 baseColor(16 单元限制) | 因子路径仍工作；带纹理的这些扩展在 Android 渲成 baseColor。真实内容影响小 | 高 |

**已核实扎实（诚实正面）**: reverse-Z 深度(near=150/far=1e12/clear=0/GEQUAL 两后端一致，planet scale 不 z-fight)、各向异性过滤+mipmap(Metal aniso4/GLES 扩展)、KHR 扩展**解析/管线**广而忠实(specular/clearcoat/sheen/transmission/anisotropy/ior/emissive_strength/unlit/pbrSpecGloss + texture_transform，*输入*甚至超 cesium 覆盖，差距纯在 BRDF *求值质量*)、法线贴图有 TBN + dFdx/dFdy 回落、多 texcoord(0..7)+4 叠加 back-to-front 排序、大气/天空背景(Rayleigh+Mie+Ozone+日轮+星空+sky-gradient clear，limb 后方正确)。

---

## 7. 3D Tiles / 内容生态 功能完整度

**状态：基础格式解码比自身 index 声称的完整得多。** 缺口在"高级生产 3D Tiles"层。

**能力矩阵摘要**（Present ✅ / Partial ◐ / Absent ✗）：

| 类别 | 能力 |
|------|------|
| **格式** | ✅ b3dm ✅ i3dm ◐ pnts(基础) ✅ cmpt ✅ glTF/glb ✗ **implicit tiling/subtree(3DT 1.1)** |
| **tileset** | ✅ tileset.json ✅ 外部/嵌套 ✅ ADD/REPLACE ✅ transforms ✅ viewerRequestVolume |
| **glTF 扩展** | ✅ mesh_quantization ✅ materials_* ✅ texture_transform ✅ webp ✅ gpu_instancing ◐ mesh_features(解析未暴露) ◐ structural_metadata(解析无运行时访问) ✗ **Draco** ✗ **meshopt** ✗ **KTX2/Basis(主动拒绝)** ✗ primitive_outline ✗ instance_features |
| **点云/样式/元数据** | ◐ 点云渲染(gl_PointSize 硬编 1.0) ✗ EDL ✗ 点样式/衰减 ✗ **3DT 样式语言** ◐ batch table(解析未暴露) ✅ feature table 解析 ✗ **per-feature 拾取** |
| **裁剪/分类/矢量/时序** | ✗ clipping planes ✗ clipping polygons ✗ 分类 ✅ GeoJSON ✗ 矢量瓦片(MVT) ✗ **贴地矢量(clampToGround)** ✅ glTF 动画 ✗ CZML ✗ 时序瓦片集 |
| **渲染功能** | ✅ 半透明 ✗ 地下/次表面 ✅ fog(剔除用) ◐ ground atmosphere(见 §6) ✅ 射线拾取 ✗ drill-pick |

**最有影响的缺失（排序）**:
1. **[P1] 压缩编解码 Draco+meshopt+KTX2/Basis** — 完全缺失（KTX2/basis/dds/astc 在 `GltfModel.cpp:464` 被**主动拒绝**）。Cesium Ion/Google Photorealistic/生产 b3dm 大量用 Draco 几何 + KTX2 纹理；无此则大批真实 3D Tiles 加载失败或无纹理。**最高杠杆**。
2. **[P1] 3D Tiles 样式语言** — 无表达式求值器，无法按属性 color/show/hide。任何 3D Tiles 数据可视化的核心。
3. **[P1] per-feature 拾取 + batch table 暴露** — batch/feature table 与 EXT_mesh_features **已解析但从不暴露**：`PickResult` 无 feature/batchId 命中类型。数据离可用只差一层管线。
4. **[P1] implicit tiling/subtree(3DT 1.1)** — QM 地形 availability-subtree 无关；无 `3DTILES_implicit_tiling`。很多现代瓦片集只发 implicit。
5. **[P2] clipping planes + 分类** — 常见分析功能，全缺。
6. **[P2] 贴地矢量线/面** — `VectorLayer` 无 `clampToGround`/heightReference；贴地矢量叠加是主流地球引擎期待。
7. **[P2] EXT_structural_metadata 运行时访问器** — 解析校验了但无 `PropertyTableView` 式读 API。

---

## 8. 架构 / 并发 / 韧性

**关键框架：工作树自 07-05 审计以来只改了 TileBoundsMetrics.cpp——没有一个 P1 correctness 项被触碰——所以几乎所有已确认未修项仍 OPEN。**

### 8.1 07-05 审计 P1 现状（全部 OPEN）

| 项 | 维度 | 严重度 | 证据 | vs 生产 |
|----|------|--------|------|---------|
| SkyBox/AtmospherePass `.release()`→裸指针，dispose 只置 null(泄漏) | 生命周期 | P1 | `SkyBox.cpp:229,253,319-323`；`AtmosphereBackgroundPass.cpp:281,301`。RenderDevice 本返回 unique_ptr，调用方用 `.release()` 破坏 | RAII；surface 重建不泄漏。每次 onSurfaceCreated 漏 1 shader+1 buffer(+cubemap) |
| GoogleMapTiles availability 向量无锁跨线程 | 并发 | **P1** | `GoogleMapTilesImageryProvider.cpp:633-642` 网线程 push_back，渲染线程 `:757,762` 读，无 `availabilityMutex_` | push_back 重分配撞遍历读 = UAF/torn read/首个 Google 影像视口崩 |
| glTF 节点树遍历无界递归→栈溢出 | 韧性 | P1 | `GltfModel.cpp:8151` `traverseNode` 自递归；~100k 链合法→SIGSEGV | 敌意输入解析器限深或显式栈迭代 |
| tileset.json `parseTile` children 无界递归 | 韧性 | P2 | `GltfContentProvider.cpp:4007-4133` 有 depth 形参但**无 `if(depth>MAX)`** | 限深 512/1024，超限作废子树不崩 |
| `SharedAssetDepot` 缓存 `std::deque::iterator`(潜在 UB) | 并发(潜在) | P1 | `SharedAssetDepot.h:189-190` deque + 存其迭代器；push_front/pop_back 使全部失效 | 模板尚未实例化(0 处)，接线即触发。改 `std::list` |
| `SharedAssetDepot::put` 已存在 key 只加不减字节 | 韧性(潜在) | P2 | `.h:58-64` existed 分支无条件 `+= assetSize` | 幻影增长→过早抖动逐出 |
| 每帧每 primitive 深拷 `RenderCommand` | 性能 | P1 | `GltfDrawCommandBuilder.cpp:371`；`RenderCommand` 仍带 3 string+2 vector+残留 uniforms map，sizeof=1664 | P0-4 常驻缓存落地但帧列表拷贝又"还回"节省(如 07-05 预测) |
| 上采样深拷整个父地形再丢弃 primitive | 性能 | P1 | `GltfTerrainUpsampler.cpp:921-922` `make_unique<GltfModel>(parent)` 后 `primitives.clear()`；深缩放每帧数十个 | 构空 GltfModel 只拷元数据 |
| `SceneRenderPipeline` 对命令列表 7-8 次全遍历 | 性能 | P1 | `SceneRenderPipeline.cpp:31,299-321,139-149`；诊断 pass 未 gate，1664B stride cache-hostile | 折叠 stat pass 进 validate/applyMvp，诊断门控 |

> **⟳ 复核更新(2026-08-17)** — 本档冻结,以下不改原表、只追加当前状态。
> 仅复核了 §8.1 里的**递归 + 并发**四条(其余泄漏/深拷/遍历项未在本轮复核范围):
>
> | 原条目 | 复核结论(2026-08-17) | 证据 |
> |---|---|---|
> | GoogleMaps availability 无锁跨线程(P1) | ✅ **已修** | 现有 `availabilityMutex_` 守两个 range vector(`GoogleMapTilesImageryProvider.h`) |
> | glTF 节点树 `traverseNode` 无界递归(P1) | ✅ **已修** | `GltfModel.cpp:5277` `kMaxNodeHierarchyDepth=256` 守卫,两处 `traverseNode` 均 `if(depth>=MAX)` 拒收 |
> | `SharedAssetDepot` 缓存 `deque::iterator`(P1) | ✅ **已消解** | `SharedAssetDepot.h` 文件已删除(该模板 0 实例化,从未接线);重写为 `QuadtreeSourceAssetDepot`=`unordered_map`+`deque<key值>`+`std::list` LRU,无缓存迭代器。残留 `SharedAssetDepot` 仅注释里指 cesium-native 概念 |
> | tileset.json `parseTile` children 无界递归(P2) | ⟳ **本轮修复** | 复核时仍无守卫;`GltfContentProvider.cpp` `parseTile` 加 `kMaxTilesetTreeDepth=512`,超限跳子树+Warning(commit `6ddcfefeb`) |
>
> **§8.1 其余 P1(泄漏 + 性能,2026-08-17 补复核):**
>
> | 原条目 | 复核结论 | 证据 |
> |---|---|---|
> | SkyBox `.release()`→裸指针泄漏(P1) | ✅ **已修** | `.release()` 零处;`shader_`/`vertexBuffer_`=`unique_ptr`,`dispose` 用 `.reset()` 真删;`cubemapTexture_` 裸指针但只赋 nullptr(cubemap 未接线,无分配) |
> | AtmospherePass `.release()` 泄漏(P1) | ✅ **已修** | 同上,`shader_`/`quadBuffer_`=`unique_ptr`+`.reset()` |
> | 每帧每 primitive 深拷 `RenderCommand`(P1 性能) | ⚠️ **仍在(部分缓解)** | 仍带 3×`std::string`(owner/pass/stableKey,`RenderCommand.h:140-144`);`uniforms` map 仅 `OffscreenPostProcess` 填充,tile 热路径为空 map(拷贝廉价) |
> | 上采样深拷整个父地形再丢 primitive(P1 性能) | ⟳ **本轮修**(等价重写) | 改为构空模型+逐字段拷 16 个非-primitive 元数据,避开 primitives 深拷(commit `70ce01c89`)。频率纠正:是 per-child 物化(churn 期几个/帧,稳态 0),非原判『每帧数十个』;端到端 ≈1.1-1.3× churn-only |
> | `SceneRenderPipeline` 多趟全遍历(P1 性能) | ⚠️ **仍在** | `aggregateDiagnostics` 每帧无门控(`SceneRenderPipeline.cpp:271`)+ applyMvp/sort/validate 多趟 |
>
> 三条性能项属**需 benchmark 驱动**的优化(触热路径),未盲改;上采样那条是低风险等价重写候选(构空模型+只拷元数据)。
>
> 取向印证:此档(07-06)的 P1 判断到 08-17 多已被后续重构消解——递归/并发四条里三条已修/消解(一条 P2 本轮修);泄漏两条已修;性能三条仍开(需测量驱动)。**复核价值在证伪与坐实**,不在照单全收。

### 8.2 并发正确性

- **✅ "detached-thread `.then` 泄漏"前提被证伪**：`ThreadPool` 用 joined worker，析构 drain，吞 worker 异常防 terminate；无 detached thread，`Future::then()` 不存在（doc 是假抽象）。池干净。
- **P3**: 池是函数局部 static 单例——静态析构序未定义（`AsyncSystem.h:129-131`）。宜由 Engine 拥有，先 join 再拆其他子系统。
- **P2**: 取消模型是逐调用点(`alive` atomic+`weak_ptr`)非一等 token；但 `RasterOverlayTileProvider.cpp:3200-3220` epoch-mismatch 分支**在池线程直接 `tile->setState(Failed)`**，撞主线程 `getState`——违反"tile state 只主线程写"不变量。宜走 pendingUpload 主线程终结。
- **✅ clip-worker 机制正确**（快照 + exactly-once guard 防槽泄漏死锁；worker 读不含 Tile/GltfModel 指针的不可变快照；已对抗式评审+真机验证）。

### 8.3 韧性

- **✅ 瞬时失败经 `FailedTemporarily` 重试，无永久子树阻塞；选择回落父/椭球；glTF 解析边界检查 → Failed 不崩**（除 §8.1 无界递归）。
- **✅ 活瓦片字节预算 LRU 卸载存在**（`TileContentCacheManager.h:55-96`，`Tileset.cpp:84` 接线）。注意 `renderer/TileTextureCache.{h,cpp}` 是**死代码**(0 引用)，真实预算在 TileContentCacheManager。
- **Gap**: 无 GPU OOM 反应——`createTexture/Buffer` 返 null 未一致检查(SkyBox 等假设成功)。
- **P2**: GLES 忽略 `blendSrc/blendDst`，硬编 `glBlendFunc(SRC_ALPHA,ONE_MINUS_SRC_ALPHA)`(`RenderDeviceGLES.cpp:774`)→加性混合内容在 Android 渲错(07-04 §5 记过仍未修)。
- **P2**: 不可信 XML 的 config 期异常未捕获——`TileMapServiceUrl.cpp:185` 解析畸形数值属性抛异常，工厂无 try/catch → 中止 init。

### 8.4 分解权衡(~279 文件 tiling/)

**裁决：净正向。** seam 有真实独立单测（`test_tile_selection_reuse_policy.cpp`/`_culling_policy`/`_refinement_policies`/`_root_policy`/`_visibility_sampler` + `test_selector_cesium_golden_diff.cpp`）；policy 类 header-only、内联、零 vtable、传引用（`submit()` 是唯一每帧虚函数）→ ~279 文件买到真测试性，运行时零间接税。
- **代价（真实但非 correctness/perf）**: `tiling/` 279 文件**扁平无子目录**；单一逻辑流(updateView→SSE)散在 ~8 跳。审计/导航税。
- **不一致**: 279 个微文件 vs 4 个 god-file 并存——`GltfModel.cpp` 8981 行、`GltfContentProvider.cpp` 4267、`RasterOverlayTileProvider.cpp` 3724、`Renderer.cpp` 2622(86% 内联 shader 串)。既过度又不足分解。宜机械 TU 拆分（行为中性，140+ 测试护航）。
- **反向分层 + include 环**: `core/geodesy/S2CellID.cpp:4`(最底层)include `tiling/TileKey.h`(上层)；6+ 双向 include 环，根在 Camera/FrameState 住 `scene/`。宜下沉 Camera/Frustum/FrameState 到低层 `view/`。

### 8.5 错误传播

- **✅ 健康混合(result-type + optional)非跨线程异常**：provider 返 `retryLater()`/typed result 不跨异步边界抛；worker 异常在池边界捕获记录保活。匹配最佳实践。
- **P3 假文档抽象误导推理**: `AsyncSystem.h:100` 记不存在的 `.then`；Persistent/HttpCache 磁盘持久是死的(`setCacheDir` 从不调)；`activeWorkerBlockingRequests_` 恒报 0。~1400 行死代码。宜"接线或删除"。

---

## 9. 建议路线图

按"高杠杆优先 + 质量优先"排序（工时按 AI 协作基准）。

### 第一梯队 — 速赢（低成本高回报，多为观感立竿见影）
1. **接线 ambient**（#2）：把已算好的 `SkyGradient::ambientColor()` 灌进 `u_ambientColor`，地形+glTF 共用；至少半球 ambient。**背光面死黑立即消失。**
2. **swapchain 层开 MSAA**（#4）：Metal `rasterSampleCount` + GLES `EGL_SAMPLES`，无需 offscreen。**边缘爬行立即消失。**
3. **实现水面着色**（#6）：把 no-op `mix(base,base,water)` 换成 mask 门控的水色+高光(用 `u_lightDir`+N)。**管线成本已付，零视觉回报→有回报。**
4. **默认打开 LOD cross-fade**（§6）：`enableLodTransitionPeriod=true`，plumbing 已在。
5. **WMS version 分支**（#13）：按 `options_.version` 切 srs/crs + BBOX 轴序。
6. **退避重试**（#10）：per-tile `nextRetryTime` 指数退避，`TileLoadRequestPlanner` 门控。**消除自造 DoS。**

### 第二梯队 — 核心保真/观感（中成本）
7. **真 Cook-Torrance BRDF**（#1）：替换 diffuse/specular 块，扩展输入已 plumb。
8. **地表 ground atmosphere + aerial perspective/fog**（#3）：把 cesium fog.glsl/aerial term 移进地形+glTF frag。**"贴平图"感 → 真 3D 地球。**
9. **HTTP 头管线 + 缓存重验**（#7）：拓宽 PlatformBridge 回调带响应头，解析 Cache-Control/ETag，条件 GET/304。**解锁 Retry-After + 缓存新鲜度。**
10. **影像走统一缓存**（#8）：所有 provider 经 HttpCache/PersistentCache。
11. **Draco + meshopt + KTX2 解码**（#5）：接入 draco/meshopt/basisu 库。**解锁真实生产 3D Tiles。**
12. **per-feature 拾取**（#12）：给 `PickResult` 加 feature/batchId 命中，暴露已解析的 batch table。
13. **修 07-05 P1 correctness**（#14/#15）：SkyBox/Atmosphere 用 unique_ptr、GoogleMapTiles 加锁、glTF/tileset 递归限深、SharedAssetDepot 改 list、RenderCommand 轻量化 handle、upsampler 只拷元数据。

### 第三梯队 — 生产功能（较大工程，按产品需要）
14. 3D Tiles 样式语言（#11）、implicit tiling、clipping planes、分类、贴地矢量、HDR+tone-mapping、阴影(CSM)、geomorph、GetCapabilities 解析、CreditSystem 接线。

### 工程卫生（行为中性，随时可做）
15. god-file 机械 TU 拆分、`tiling/` 子目录分组、下沉 Camera/FrameState 到 `view/` 破 include 环、删/接~1400 行死代码与假文档、`createFramebuffer` 实现（解锁 AA/HDR/阴影全部后处理）。

---

## 附录 A — 对既有文档/记忆的纠正

1. **AI_INDEX 低估内容管线**：只写地形 glTF，实为完整 b3dm/i3dm/pnts/cmpt/glb 加载器 + tileset.json + glTF 动画 + GPU 实例化。建议重写 AI_INDEX §8。
2. **`AsyncSystem::Future::then` 不存在**（stale memory 说它泄漏 detached thread）：类里只是 `std::future` 包装，`.then` 是从未实现的假文档。池是干净的有界 joined 池。
3. **`renderer/TileTextureCache` 是死代码**（0 引用）：真实字节预算在 `TileContentCacheManager`。
4. **`createFramebuffer` 是 stub**（两后端返 nullptr）：这是 AA/HDR/阴影/bloom 全部缺失的共同根因，是后处理的单一解锁点。

## 附录 B — 调研方法

7 路并行子代理，各自读 AI_INDEX 对应节 + 源码后逐点对齐 cesium-native：(1)瓦片选择/LOD (2)内容流式/缓存/GPU上传 (3)地形 (4)影像叠加 (5)渲染质量 (6)3D Tiles 功能完整度 (7)架构/并发/韧性(含分解权衡子代理)。每项差距标注 file:line 证据、维度、严重度、vs Cesium、修复方向、置信度。诚实正面项单列以防误判为差距。性能判断均以 release(-O2) 为准（debug -O0 数值膨胀，见 memory `perf-measured-on-debug-build`）。
