# 数据处理→上屏链路调研与审计（2026-07-17）

> 背景：Android demo 真机性能一般，判断瓶颈不在 selector，而在"数据加载完成后的处理与上屏"。
> 方法：四路并行——cesium-native、CesiumJS + 3DTilesRendererJS、MapLibre + osgEarth/OSG（均为 `.ref/` 本地源码逐行分析，非凭记忆）+ 自研链路 A–H 全跳审计（按 feat/offscreen-render-pass 当前工作树）。
> 口径：所有耗时判断以 release 为准（历史 debug 数字膨胀 2-3×，见 perf-measured-on-debug-build）。本报告为静态审计，各问题的真实 ms 需 release 插桩确认后再修。

---

## 0. TL;DR

用户的方向判断**成立**：数据上屏路径确实存在结构性瓶颈，且与 selector 无关。但瓶颈形态和直觉略有出入：

1. **不是"闸门开太小"**——`finalize=1/rasterUpload=1` 只是 struct 默认值，生产路径每帧被 `TileFrameResourceBudgetPlanner` 覆盖为：平滑期 finalize=4、rasterUpload=13，静止 20/20，另有恒定 8ms 主线程墙钟总闸。
2. 真正的问题是三层叠加：
   - **上传本身太贵**：GLES 全同步（`glTexImage2D`/`glBufferData`/`glGenerateMipmap` 主线程裸跑），且传输字节量是必要值的 1.5-2×（未压缩 RGBA8 纹理 + float32 40B 地形顶点 + 恒 uint32 索引）；
   - **8ms 墙钟被所有资源工作共享**：地形打包 + 地形 GPU drain + 影像上传竞争同一个预算，风暴期地形吃满 → 影像饿死；
   - **稳态每帧固定税**：每帧对每个可见瓦片深拷贝胖 `RenderCommand`（含 3 个 `std::string` + map/vector），数百可见瓦片时是持续堆分配洪流。
3. 成熟引擎的共同解法不是"更大的计数闸"，而是：**重活全下 worker + 主线程只留窄口子 + 时间预算（非计数）在更细粒度上摊薄 + 字节减量（量化/压缩/上传即释放）**。我们在"重活下 worker"上已经基本对齐（decode/法线/skirt/clip 都在 worker），差距集中在后三项。

---

## 1. 前提纠偏：生产闸门的真实形态

`FrameResourceBudgetConfig` 的 `maxMainThreadFinalizesPerFrame=1`、`maxRasterUploadsPerFrame=1`（FrameResourceBudget.h:35/37）**从不进入生产**：`Tileset` 构造与每帧 `beginFrame` 都被 `TileFrameResourceBudgetPlanner::plan()` 覆盖（Tileset.cpp:38、TileUpdateFrameContextBuilder.h:60-73）。真实值（默认 `mainThreadLoadingTimeLimit=8.0`、`maximumSimultaneousTileLoads=20`，Tileset.h:61/116）：

| 状态 | finalize/帧 | rasterUpload/帧 | 墙钟 |
|---|---|---|---|
| 拖动中 / 停手后 1.25s 平滑窗 | ⌈8/2.5⌉=**4** | ⌈8/0.625⌉=**13** | 8ms 共享 |
| 静止 | 20 | 20 | 8ms 共享 |

（TileFrameResourceBudgetPlanner.h:95-127，抽查核实无误；平滑窗判定 TileFrameInteractionTracker.h:43-50）

另一个关键事实：**拖动中非 Urgent 地形 finalize 完全冻结**（TilePendingLoadQueue.cpp:149-152 队首非 Urgent 即返回空）→ 风暴积压全部推到停手后，以 4/帧 + 8ms 墙钟消化 → "停手后地表逐块补齐"的观感由此而来。

---

## 2. 成熟引擎怎么做（四库交叉验证）

### 2.1 线程切分：主线程只留"GPU 对象创建"一个窄口子

- **cesium-native**：glTF 解析、Draco/meshopt/KTX2 解码、图像解码、反量化、平滑法线、overlay UV 生成、上采样、宿主 `prepareInLoadThread` 全在 worker（GltfReader.cpp:258-389、TilesetContentManager.cpp:527-636）；主线程只做 `setTileContent` + 宿主 `prepareInMainThread`（:1656-1662）。上采样在 worker **按引用捕获父 Model**，靠"父瓦片存活到子内容返回主线程"的生命周期契约避免拷贝（TilesetContentManager.cpp:1180-1215、RasterOverlayUpsampler.cpp:81-95）。
- **CesiumJS**：quantized-mesh 反量化/三角化/skirt/TerrainEncoding 全在 web worker（QuantizedMeshTerrainData.js:240-309）；主线程只做 VertexArray 创建。图像走 `createImageBitmap` 浏览器异步解码（Resource.js:1965-2050）。
- **MapLibre**：PBF 解析、三角化、bucket 构建、symbol layout 全在 worker（worker_tile.ts:63-208）；主线程只做 `bufferData`。**它甚至没有上传节流**——重活全部前移后主线程剩余工作便宜到不需要节流（tile_manager.ts:220-230）。
- **OSG DatabasePager**：文件解析、节点构建全在 pager 线程；合入场景图只剩 `addChild` 指针操作，天然便宜到不用限流（DatabasePager.cpp:885-941、1607-1710）。

> **启示（MapLibre 的对照最锋利）**：节流与 worker 前移是**可互相替代的设计维度**。主线程剩的活越便宜，闸门越不重要；反之闸门再精巧也治不了"单次上传本身贵"。

### 2.2 上传摊薄：时间预算 > 计数预算，粒度越细越稳

- **CesiumJS JobScheduler**（摊薄粒度 = 单个 texture/buffer/program，JobScheduler.js:67-198）：
  - 分类型毫秒预算：TEXTURE 10ms / PROGRAM 10ms / BUFFER 30ms，每帧 reset；
  - **预算偷取**：某类型用尽可偷其它类型的本帧余量（上帧被饿死的类型除外）；
  - **每类型每帧保底跑 1 个**：防止贵纹理饿死 shader 编译；
  - 超预算 → loader `process()` 返回 false，下帧重试（GltfTextureLoader.js:168-172）。
- **CesiumJS 地形队列**：5ms 墙钟时间片 + `didSomeLoading` 保底（`i < len && (now < endTime || !didSomeLoading)`，QuadtreePrimitive.js:1328/1373-1382）——"超时切断但至少推进一个"是防饿死死锁的现成模式。
- **OSG IncrementalCompileOperation**（跑在绘制线程，IncrementalCompileOperation.cpp:663-730）：
  - 从目标帧率反推本帧可用时间：`availableTime = max((targetFrameTime - elapsed) * 0.5, 1ms)`——帧有余量多传、帧紧张自动收紧；
  - **编译与删除共享同一预算池**（flushTime/compileTime 分账，flush 没用完回滚给编译）；
  - 时间 + 对象数（默认 20/帧）双门控；本帧编不完的对象留游标下帧续。
- **cesium-native**：`mainThreadLoadingTimeLimit` 时间预算 + 优先级排序队列（Urgent>Normal>Preload，组内按距离/SSE）+ 每帧至少完成 1 个（时间检查在 `finishLoading` 之后，TilesetContentManager.cpp:1944-1961）。粒度 = 整瓦片（一次 `prepareInMainThread` 创建该瓦片全部 GPU 资源）——**分片摊薄留给宿主**，cesium-unreal 等宿主自己做细粒度。
- **MapLibre PauseablePlacement**：2ms 预算 + 双层游标（tile/part）可恢复状态机，超时存游标下帧续跑（pauseable_placement.ts:90-128）——"游标+预算+resume"的通用壳。

### 2.3 字节减量：量化直传 + 压缩纹理 + 小索引

- **CesiumJS TerrainEncoding BITS12**：包围盒 <4095m 时 position+height 位打包压 stride，shader 里 decode matrix 反量化（TerrainEncoding.js:40-109/387-429）；纹理坐标两分量压一个 float。
- **glTF KHR_mesh_quantization**：量化小类型（normalized SHORT/BYTE）**直传 GPU**，dequantize 全在顶点着色器——"传输态=GPU态"，中途不展开成 float（GltfLoader.js:1008-1100）。
- **cesium-native KTX2 转码管线**：Basis→按平台/通道数转 ETC2/ASTC/BC7 等硬件压缩格式，mip 拼单一连续 buffer 一次上传（ImageDecoder.cpp:91-172）。
- **osgEarth**：纹理 CPU 压缩（S3TC/ETC2）+ mipmap 生成全在后台线程、瓦片模型创建阶段就完成，交到 GL 手里已是最终 GPU 格式（TerrainTileModelFactory.cpp:702-737）。

### 2.4 拷贝纪律与对象回收

- **MapLibre StructArray**：worker 内按**最终 GPU 字节布局**一次写好 → postMessage transferable 零拷贝移交 → `bufferData` 后**立即 `freeBufferAfterUpload()` 释放 CPU 副本**（struct_array.ts:206-209、vertex_buffer.ts:39-53）。全程 JS 层零冗余拷贝。
- **cesium-native**：整 Model 全程 `std::move`，网络→Model 仅 1 次实拷贝（GltfReader.cpp:250-253/627-629）。CPU Model 故意保留（供上采样/高度采样/UV 重算）——保留与否是宿主决定。
- **osgEarth GLObjectPool**：GPU 对象**回收优先于删除**（同尺寸/同格式 VBO/纹理直接复用，免 glDelete+glGen 抖动）；删除走 100KB/帧字节预算 + 30 帧延迟（GLUtils.cpp:531-533/714-745）。MapLibre 同理有按尺寸分桶的瓦片纹理回收池（painter.ts:731-747），同尺寸纹理走 `texSubImage2D` 原地更新。
- **osgEarth 合入配额只计"真正带几何体"的项**（PagedNode.cpp:492-506）——节流按"合入代价"计数，不按"合入次数"，防止廉价项吃满配额。

### 2.5 六引擎 × 我们 对照表

| 机制 | cesium-native | CesiumJS | MapLibre | osgEarth/OSG | **我们** |
|---|---|---|---|---|---|
| 重活下 worker | ✅ 全量 | ✅ 全量 | ✅ 全量 | ✅ 全量 | ✅ 基本对齐（残留：upsample 父模型主线程深拷、prepareCpuWork 主线程冗余拷贝） |
| 主线程摊薄形态 | 时间预算/整瓦片 | 时间预算/**单资源** | 无（不需要） | 时间预算/单对象+游标 | 时间(8ms 共享)+计数/整瓦片 |
| 分类型预算隔离 | — | ✅ 10/10/30ms+偷取+保底 | — | 编译/删除同池分账 | ❌ 全类型共享 8ms |
| 顶点量化直传 | ✅(KHR_mesh_quant) | ✅(BITS12) | ✅(int16 布局) | — | ❌ float32 40B |
| 压缩纹理 | ✅ KTX2→ETC2/ASTC | ✅ | — | ✅ CPU 压缩+后台 mip | ❌ RGBA8 裸传 |
| uint16 索引 | ✅ | ✅ | ✅ | ✅ | ❌ 恒 uint32 |
| 上传即释放 CPU 副本 | 宿主决定 | ✅ | ✅ freeBufferAfterUpload | ✅ | 部分（terrainGpuVertexBytes 已延迟释放✓；SurfaceVertex 双份 208B/顶点常驻） |
| GPU 对象回收池 | 宿主决定 | 部分 | ✅ 纹理池 | ✅ recycle+删除预算 | ❌ |
| 异步上传(PBO 等) | 宿主决定 | 浏览器托管 | 浏览器托管 | 部分(ICO 在绘制线程) | ❌ 全同步 GL |

---

## 3. 自研链路 A–H 审计结论（浓缩）

完整链路：网络字节 →(A)worker 解析 →(B)prepare 切分 →(C)finalize 闸门 →(D)GLES/Metal 上传 →(E)raster 上传 →(F)draw command →(G)CPU 副本 →(H)每帧重复工作。

- **A ✓（P2）**：QM 解析/ECEF/法线/skirt/重投影全在 worker 池，主线程零解析。但 worker 内有 3 次 SurfaceVertex(104B/顶点) 全量拷贝（QuantizedMeshContentLoader.cpp:85/111/117）+ 逐顶点迭代测地反投影（:155-248）——不卡帧，但池饱和时拉长可见延迟。
- **B ✗（P1）**：`prepareCpuWork` 名义上是 Phase1/worker，**实际在主线程**被 `ensureGltfResources` 同步调用（TilesetContentLifecycleCoordinator.h:161-221→GltfRenderResourcePreparer.cpp:419/457），把 worker 已预建的顶点/索引字节在主线程再整拷一次进 GpuReadyData。更重的是 **upsample `buildClipInput` 在主线程深拷整份父 GltfModel**（TileGltfTerrainUpsampledChildMaterializer.h:88，调用点 TileLoadScheduler.h:229）——先全拷再 prune，部分白拷；深缩放时是主线程尖刺。（clip 主体已 worker 化 ✓）
- **C ✗（P1）**：一次 finalize 粒度 = 整瓦片全部 buffer+texture；**8ms 墙钟被地形打包、地形 GPU drain、影像上传全局共享**（TileUpdateUploadRunner.h:52/61、FrameResourceBudget.cpp:100/153）→ 风暴期地形吃满 8ms 饿死影像。瓦片须顺序过两道闸（打包闸→drain 闸，后者另有 floor 2/4 + backstop 20，TilesetContentLifecycleCoordinator.h:272-284）。拖动中非 Urgent finalize 全冻结（见 §1）。
- **D ✗（P1，最根本）**：GLES 上传全同步无 PBO/staging（RenderDeviceGLES.cpp:250/287/370/386），`glGenerateMipmap` 主线程；纹理仅 RGBA8/RGB8/R8 未压缩（:227-245）；地形顶点 40B float32 无量化（GltfRenderGeometryBuilder.h:33-41）；**索引恒 uint32**（GltfRenderResourcePreparer.cpp:711）。传输量 1.5-2× 于必要值，直接吃掉 C 的 8ms。Metal 侧较好（blit 队列 mipmap + shared storage）。
- **E ✓（P2）**：解码/合成全 worker，mapped 路径已关 mipmap/edge-bleed，主线程收敛到纯 `glTexImage2D`（256KB/张）。风险只剩与地形共享 8ms（归 C）。
- **F ✗（P1）**：常驻命令缓存已落地 ✓，但**每帧对每个可见瓦片每 primitive 深拷贝整个胖 `RenderCommand`**（GltfDrawCommandBuilder.cpp:458 `commands.push_back(cachedCommand)`）——含 3 个 `std::string`（owner/pass/stableKey，stableKey 非空必堆分配）+ uniforms map + 2 个 vector（RenderCommand.h:43-100）。数百可见瓦片 → 每帧数百次 ~1KB 拷贝+堆分配洪流，稳态成本随可见数线性增长。历史"50ms trim 抖动"证伪，现状是拷贝本身。旁边已有 `commandCopyMs` 插桩可直接测量。
- **G 部分✓（内存 P1/卡顿 P2）**：`terrainGpuVertexBytes` 已延迟释放（worker 池，32MB 限额）✓；但 `vertices`+`runtime.baseVertices` 双份 double SurfaceVertex（共 208B/顶点）**永不释放**——是 upsample 深拷与高度采样的数据源，即历史 ~247MB 常驻仍成立，与 B 的主线程深拷互相放大。
- **H ✓（P2）**：raster 映射已收敛"加载时一次"+跨帧复用闸 ✓；残留每帧 O(N log N) 全排序 + loadRequests 拷贝重排 + 多个 unordered_set 重建（TileRasterOverlayFrameProcessor.h:160-172/233-247）。工作树新增的 early-map 有 4/帧+1ms 独立闸，受控非回归。离屏渲染改动未在上屏热路径引入同步点 ✓。

---

## 4. Top 问题清单（按疑似真机卡顿贡献排序）

| # | 严重度 | 问题 | 锚点 |
|---|---|---|---|
| 1 | P1 | GLES 上屏全同步、无 PBO/异步传输，mipmap 主线程 | RenderDeviceGLES.cpp:250/287/370/386 |
| 2 | P1 | 8ms 墙钟为地形 finalize+打包+drain+影像上传全局共享，风暴期互相饿死 | FrameResourceBudget.cpp:100/153、TileUpdateUploadRunner.h:52/61 |
| 3 | P1 | 每帧逐可见瓦片深拷胖 RenderCommand（string+map+vector） | GltfDrawCommandBuilder.cpp:458、RenderCommand.h:43-100 |
| 4 | P1 | upsample buildClipInput 主线程深拷整份父 GltfModel | TileGltfTerrainUpsampledChildMaterializer.h:88、TileLoadScheduler.h:229 |
| 5 | P1 | 未压缩纹理 + float32 40B 顶点 + 恒 uint32 索引 → 传输量 1.5-2× | RenderDeviceGLES.cpp:227-245、GltfRenderGeometryBuilder.h:33、GltfRenderResourcePreparer.cpp:711 |
| 6 | P1 | 拖动中非 Urgent finalize 全冻结 → 停手后按 4/帧消化积压，地表逐块补齐 | TilePendingLoadQueue.cpp:149-152 |
| 7 | P2 | 地形预建字节主线程冗余整拷一次（打包）+ createBuffer 再拷（上传） | GltfRenderResourcePreparer.cpp:419/457 |
| 8 | P2 | 双份 double SurfaceVertex（208B/顶点）常驻永不释放（~247MB 级） | GltfModel.h:110/120 |
| 9 | P2 | prefetchSelection 每帧全排序 + 拷贝重排 + set 重建 | TileRasterOverlayFrameProcessor.h:160-172/233-247 |
| 10 | P2 | QM decode 三重 SurfaceVertex 拷贝 + 逐顶点迭代反投影（worker 内） | QuantizedMeshContentLoader.cpp:85/111/117/155-248 |

---

## 5. 修复方向（按"最干净/最对"排序；每项附风险；未经用户拍板不动手）

先说总纲：MapLibre 的对照说明**先把主线程剩余的活变便宜，再谈闸门精巧化**。以下前三项都是"把活变便宜"，第四项才是闸门重构。每项都给可验证信号（release 插桩）。

**方向一：字节减量三件套（对应 #5，部分缓解 #1/#2）**
uint16 索引（<65536 顶点时，几乎所有地形瓦片满足）→ 地形顶点量化直传（uint16 pos/normal + shader dequantize；cache-10x 调查已确认"GPU 量化+uint16 索引管道现成"，本质是接线）→ raster 纹理 ETC2 转码（worker 内 JPEG→RGBA→ETC2）。
- 收益：索引带宽 −50%；顶点 40B→~16B；纹理 256KB→64KB（ETC2 4bpp），上传墙钟等比例缩短。
- 风险：量化精度需 bit-exact 验证（skirt/接缝敏感，历史有 z-fighting 教训）；ETC2 实时编码在 worker 有每张 ~ms 级成本且有质量损失（高德影像色带风险），需真机 A/B；Metal 侧格式路径要同步（ASTC/BC 分叉）。
- 验证信号：drain/rasterUpload 的 recordElapsed 均值下降 ≥40%；golden 像素对比（量化前后）。

**方向二：RenderCommand 瘦身（对应 #3，历史 P0 病灶的残留形态）**
缓存命令改 POD/句柄化（string→interned id 或定长 key；uniforms map 出热路径；resourceKeepAlive 不随命令每帧拷），每帧只 patch 每帧态。
- 收益：稳态每帧数百次堆分配→0；对"静止也不满帧"的场景直接有感。
- 风险：动两后端契约（Metal/GLES 都消费 RenderCommand），改动面广；pass/owner 字符串有调试用途需保留映射表。
- 验证信号：已有的 `commandCopyMs` 插桩（GltfDrawCommandBuilder.cpp:457-462）release 下前后对比。

**方向三：upsample 主线程深拷下放（对应 #4，捎带 #7）**
学 cesium-native 契约：父瓦片存活保证 + worker 按引用/共享指针捕获父 Model 只读快照，深拷（如仍需要）移进 worker；`prepareCpuWork` 的冗余整拷改 move。
- 收益：深缩放期主线程尖刺消除；#7 顺手消掉。
- 风险：悬垂问题历史上被对抗评审否决过裸指针方案——必须做成"存活到子内容返回主线程"的显式 pin（引用计数/keep-alive），并防父内容被 cache 驱逐；exactly-once guard 与现有 requestContent 复用机制的交互要回归（clip-worker-ization 的契约一二不动）。
- 验证信号：深缩放场景主线程 buildClipInput 计时 →≈0；native 全绿 + golden 不变。

**方向四：预算演进——分类型时间预算 + 资源粒度摊薄（对应 #2/#6，JobScheduler/ICO 形态）**
8ms 总闸拆分账户（地形 buffer / raster 纹理 / 其它），允许偷取 + 每类保底 1 个；finalize 粒度从整瓦片下探到单 buffer/texture job（需要游标式可恢复状态 + "瓦片全资源就绪才可见"的簿记）；拖动期冻结改为涓流（保底 1-2 个/帧）。
- 收益：饿死消除、风暴削平、停手补齐加速。
- 风险：这是四个方向里改动最深的——半上传瓦片状态机复杂度高（cesium-native 都没做，留给宿主）；拖动涓流可能回引拖动微卡顿，需真机 A/B（历史上交互期上传就是反复调过的敏感区）；建议放在方向一之后做，字节减量后 8ms 能塞下的资源数本身会翻倍，闸门压力先天减半。
- 验证信号：风暴场景（斜视地平线）停手→全清晰的帧数；影像上传在地形风暴期不再归零。

**低优先但记录**：GPU 对象回收池（osgEarth recycle 形态，治换入换出 churn）；SurfaceVertex 常驻精简（cache-10x 路线图既有方向：精简常驻 uint16 u/v/h，做了方向三后它就是纯内存问题）；prefetchSelection 增量化（P2 稳态税）。

---

## 6. 已核实、无需重查的清单（防止未来重复审计）

- `finalize=1/rasterUpload=1` 是 struct 默认值非生产值（生产平滑期 4/13、静止 20/20）——**以后别再引用"每帧 1 个 finalize"**。
- clip 已 worker 化 ✓（残留仅主线程快照深拷）；`terrainGpuVertexBytes` 已延迟释放 ✓（commit 164b03790）；raster 首次映射"加载时一次"+复用闸 ✓；raster 解码/合成在 worker ✓；常驻 draw command 缓存已落地 ✓（问题是每帧拷贝而非重建）；历史"buildRenderCommands 50ms trim 抖动"= debug 假象，现状为拷贝成本。
- 离屏渲染工作树改动未在上屏热路径引入同步点（无 glFinish/回读；显式 glFlush 已删）。
- QM 解析/重投影/法线/skirt 全在 worker，主线程零解析。
- Metal 后端上传路径较健康（blit mipmap + shared storage），Android/GLES 才是主战场。
- `.ref/` 口径：maplibre 是稀疏检出（缺 src/data 等，调研时按同 commit 远程补齐）；osgearth 仓库不含 rex 引擎插件源码（结论来自通用 PagedNode2/GLObjectsCompiler + OSG 核心）。
