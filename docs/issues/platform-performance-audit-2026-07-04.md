# 平台层架构与策略性能审计（2026-07-04）

**分支**：`codex/surface-instancing-gpu-batch`
**修复进度（2026-07-04 晚）**：✅ P0-1/P0-2/P0-3/P0-4、P1-1/P1-2/P1-3/P1-9、P2-1/P2-3/P2-5/P2-8/P2-10/P2-11/P2-13/P2-14/P2-16/P2-17/P2-22 及 P2-20 的空 handler 部分已修复入库（commits 8d4d79e1f…ffcf1a141；P0-4 修于 2026-07-04 深夜）。真机 Adreno 730 实测：GLES submit 段 2.77ms→0.50ms（-82%，uniform 句柄化+VAO 合并效果，达成 §6 验收线）；Metal 完整 glTF PBR PSO 首次可创建；P0-4 常驻命令缓存后 render（命令构建）段稳态 5.3-7.5ms→4.3-7.0ms，截图像素级一致。未修：P2-2/4/6/7/9/12/15/18/19/21 及 P2-20 状态去重部分（P0/P1 已全部修复,P1-10 修于 2026-07-05 凌晨）。P1-7/8(网络与解码链路)亦修于同晚:HttpCache shared_ptr 零拷贝+字节预算+put 移出网络线程;Android JNI 句柄缓存+线程生命周期 attach(修无条件 Detach 正确性隐患),真机 343 次解码零异常、截图与基线一致。P1-4/5/6(Metal 帧调度三件套)修于 2026-07-04 深夜:drawable 超时+in-flight 信号量+updateBuffer orphan+mipmap blit,MTL_DEBUG_LAYER 0 错误、140/140、macOS 截图与基线一致;顺带完成 P2-20 的"drawable 存 Impl 成员"部分。
**方法**：四路并行只读审计（GLES 后端 / Metal 后端 / 网络桥接层 / 帧循环与上传策略层），交叉核对后合并。
**范围**：`platform/`（RenderDeviceGLES、RenderDeviceMetal、CurlMultiRequestScheduler、Android/iOS/Mac 桥）+ 与平台层耦合的中层（RenderCommand 契约、SceneRenderPipeline、上传预算/节流策略、HttpCache）。
**不含**：已修项（相机高空早退、budget lane 涓流、节流令牌、touchInertia、glFlush 移除、GLES sampler 压缩等，见 MEMORY）；低空高度查询 O(瓦片×三角形) 为已知 TODO 未重复。

---

## 0. 总结论：一个架构病灶主导 CPU 帧成本

**`RenderCommand::uniforms = unordered_map<std::string, std::vector<float>>`（RenderCommand.h:83）是全链路的单一最大性能税**，三路渲染审计独立收敛于此。它把"uniform 布局"这个编译期可知的信息推迟到每 draw 每帧用字符串哈希重新发现，锁死了四个成本：

1. **上游构建**：每 tile 每帧从零构造命令，glTF/terrain 命令携带 ~100 条 map 条目（多数键 >22 字符超 SSO 必堆分配），每命令 ~150-200 次堆分配（GltfDrawCommandBuilder.cpp:45-409、Renderer.cpp:2475-2649）。
2. **中游拷贝放大**：命令被深拷贝 3 次——RenderCommandStreamingSet::update:22 拷入 → :40 拷出 → SceneRenderPipeline.cpp:303-306 insert 拷入帧列表。每次深拷贝复制整个 map（~300 次分配）。**50 tile 时估算每帧 5 万+ 次 malloc/free**。
3. **GLES 后端**：每 draw 遍历整个 map（~100 次 string hash + ~100 次 glUniform*，无脏值过滤）+ 循环内 `"u_mappedRasterTexture"+to_string(i)` 现场拼接。60 命令 ≈ 每帧 ~6000 次哈希 + GL 调用，估 1-3ms 驱动开销（RenderDeviceGLES.cpp:745-842）。
4. **Metal 后端**：每 draw 逐槽绑定表 ~90 次 `uniforms.find` + ~30 次 setBytes，估 100 draw ≈ 1-4ms 纯编码 CPU（RenderDeviceMetal.mm:602-825）。**副作用**：绑定表被迫铺到 `[[buffer(85)]]`，超出 Metal 每 stage 31-buffer 硬上限 → **完整 glTF PBR 管线在 Metal 上根本创建不出 PSO**（Renderer.cpp:1577-1578，:1960 注释自认）。

**现成逃逸先例**：SurfaceTile 命令的 `hasSurfaceTileUniforms` 定长块（RenderCommand.h:93-115）就是正确模式。终局方案：按 shader layout 定义 POD uniform block，命令存 block 本体；后端 Metal 一次 setBytes、GLES 走 UBO + location 数组直传。此一项同时消掉 1-4 全部四个成本和 Metal 31-buffer 死胡同。

**分层本身是健康的**：tiling/ policy 类几乎全 header-only 模板 + 引用传参，编译期内联无虚调用；`submit(RenderCommandList)` 是每帧一次虚调用而非每命令一次。策略分层不是天花板，命令表示才是。

---

## 1. P0 发现（架构级 / 高杠杆）

### ✅[已修复 f9e027d8d] P0-1 RenderCommand uniform string-map（横切，见 §0）
修复方向：POD uniform block + 材质句柄；StreamingSet 改真缓存；列表传递 move 化。

### ✅[已修复 f9e027d8d] P0-2 Metal glTF 绑定表超 31-buffer 硬上限
`RenderDeviceMetal.mm:809-824` / `Renderer.cpp:1577-1578`。glTF fragment MSL 声明到 buffer(85)。Metal 上完整 glTF 路径当前不可用（terrain 轻量 shader 是唯一活路），且 84 槽死表对 Tile/Color shader 每 draw 白跑 ~90 次哈希。修复与 P0-1 同体：材质参数合并 1-2 个 struct buffer。

### ✅[已修复 8d4d79e1f] P0-3 curl_multi_wait 唤不醒 + DNS 阶段忙旋（一行修）
`CurlMultiRequestScheduler.cpp:335` 用 `curl_multi_wait`，但 `curl_multi_wakeup`（:106、:273）按 curl API 契约**只作用于 `curl_multi_poll`**。只要有传输在飞，新请求/取消平均白等 25ms（最多 50ms）。同一行还有第二个坑：numFds==0（DNS 解析/建连窗口）时 `curl_multi_wait` 立即返回，无补睡 → 网络线程满核自旋。**修复：`curl_multi_wait` → `curl_multi_poll`，两个问题同灭。**

### ✅[已修复 2026-07-04] P0-4 渲染命令每帧从零重建 + StreamingSet 是假缓存
`RenderCommandStreamingSet.cpp:21-42`：每帧无条件用新 candidate 覆盖 entry——它不是缓存，是纯开销的复制中转站（输出≡输入）。地形瓦片命令内容 100% 可跨帧复用（只有 MVP/light/opacity 每帧变），与 cesium-native"per-tile 常驻 DrawCommand + 每帧只更 per-frame uniform"背离。`stableKey` 基础设施本为缓存而生（每帧还付字符串拼接成本）却没被用作缓存。

**修复**：常驻命令缓存下沉到 `TileRenderContentState`（cesium per-tile DrawCommand 语义，生命周期与内容资源严格对齐，mutator 即失效点）；`GltfDrawCommandBuilder` 拆为"内容不变式重建（仅缓存失效时）"+"每帧盖章（frameId/generation/opacity+blend 派生/clip/overlay 绑定，盖在帧列表副本上）"；`RenderCommandStreamingSet` 删除，tileset 命令直接追加帧列表——每帧 3 次深拷贝→1 次、非 clip stableKey 字符串构建移入缓存重建（每帧零字符串）。验证：140/140 native（新增 test_gltf_draw_command_cache 锁缓存/失效/每帧盖章语义）+ macOS/Android 真机 A/B 截图像素级一致 + Adreno 730 render 段稳态 5.3-7.5ms→4.3-7.0ms。

---

## 2. P1 发现（每帧热路径，确认）

### GLES 后端
- ✅[已修复 ffcf1a141] **P1-1 无 VAO**（RenderDeviceGLES.cpp:507-712）：每 draw 全量重发最多 15 个 glVertexAttribPointer + divisor，即使连续命令同 VBO 同布局；兜底分支 divisor 无条件重发。~2000-3000 GL 调用/帧仅顶点状态，估 0.5-2ms。（此项在 android-performance-analysis.md 已列 P1，**已知未修，本次确认仍在**。）
- ✅[已修复 f9e027d8d] **P1-2 sampler + 材质常量每 draw 重传**（:745-771、:818-842）：sampler uniform 是 program 持久状态却每 draw 重发 ~18 个 glUniform1i；~45 条 per-material 常量每帧重传即使一字未变。修复：sampler 挪 createShader；材质常量迁 UBO。
- ✅[已修复 f9e027d8d] **P1-3 uniformLocation(const std::string&) 隐式临时 string**（RenderDeviceGLES.h:100 + 调用点全传 const char*）：每帧 ~3000-5000 次 hash 查找 + ~1000-2000 次堆分配。修复：program 创建时解析为定长 GLint 数组按枚举索引。

### Metal 后端
- ✅[已修复 2026-07-04] **P1-4 nextDrawable 帧首获取 + `allowsNextDrawableTimeout=NO` + 主线程渲染**（RenderDeviceMetal.mm:523、MetalView.mm:33/106）：GPU 落后时无限期阻塞 UI 线程，整段编码期间握着 drawable（池仅 3 个）。修复：layer 允许 timeout（nil 即跳帧）+ beginFrame in-flight 闸门带 1s 超时跳帧——两侧都不再无限期阻塞主线程。
- ✅[已修复 2026-07-04] **P1-5 无 in-flight 帧信号量，shared buffer CPU 写与 GPU 读裸竞争**（RenderDeviceMetal.mm:251-263 + GltfRenderResourcePreparer.cpp:174-186）：动画 glTF 每帧原地覆写在飞 vertex buffer。修复：in-flight 信号量（kMaxFramesInFlight=2，completed handler 归还，surface 销毁路径 commit 防名额泄漏）+ updateBuffer 改 orphan 式换存储（已提交 command buffer 强引用旧 id，比 ring buffer 简单且对部分更新也正确）。GLES 侧同构问题（glBufferSubData 覆写在用 buffer）未动，驱动 ghost-or-stall 兜底，留待 profile 证明有感再改。
- ✅[已修复 2026-07-04] **P1-6 Metal mipmap 从未生成**（RenderDeviceMetal.mm:176-195）：按 mip 链分配（显存+33%）+ 三线性采样，但无任何 generateMipmaps blit——GLES 有（RenderDeviceGLES.cpp:215），Metal 漏实现。缩小采样读未初始化 mip（正确性）。修复：createTexture 上传 level 0 后 blit generateMipmaps，语义与 GLES 对齐（仅 create-with-data 生成；影像瓦片纹理均为 create-with-data，覆盖热路径）。

### 网络桥
- ✅[已修复 2026-07-04] **P1-7 HttpCache 全局单锁 + 锁内整 body 深拷贝 + 在唯一网络线程执行 put**（HttpCache.h:62-91、171-178；GltfContentProvider.cpp:2283）：命中锁内整拷 body；put 全链拷 3 次；每瓦片 ~3×75KB 拷贝串在 curl 线程上头部阻塞后续传输。附带：容量按条数（2000）不按字节 ≈150MB+ 内存风险。修复：条目存 shared_ptr<const CachedResponse>（命中零拷贝、锁内零分配）、put 全链 move、persistAsync 捕获同一 shared_ptr、加 128MB 字节预算双限；requestBodyAsync 的 put 挪到池线程并与解码共享同一份 body。新增 test_http_cache 锁语义（共享存储/字节记账/LRU/过期）。
- ✅[已修复 2026-07-04] **P1-8 Android decodeImage 每瓦片 JNI 全税**（AndroidPlatformBridge.cpp:296-396）：每张图 AttachCurrentThread/**无条件 Detach**（若线程本已 attached 会被摘下，正确性隐患）+ 10+ 次 FindClass/GetMethodID 无缓存 + 3 次大拷贝。修复：jclass/jmethodID/枚举常量一次性解析缓存（global ref，magic static）+ 线程生命周期 attach（pthread key 析构 detach，已附着线程绝不摘）+ 无 padding 时整块拷贝。真机 343 次解码零 JNI 异常。未做（幅度需 profile 再定）：Java Bitmap inBitmap 复用池、native 解码（libjpeg-turbo）。

### 策略层
- ✅[已修复 b77e5e701] **P1-9 上传预算计数制、时间闸门出厂即关**（Tileset.h:72 `mainThreadLoadingTimeLimit=0.0` → FrameResourceBudget `mainThreadTimeExpired()` 恒 false；TileFrameResourceBudgetPlanner.h:111-121）：相机静止 1.25s 后 smoothing 关闭，一帧允许 20 次同步 finalize + 20 次 raster 上传，**瓦片批量到达正好落在无节流窗口 → 静止帧尖峰**（与已修的"交互卡顿"互补的另一半）。且全部 lane 的 `estimatedCostUnits=1`：1024² mipmap 纹理与 4KB mesh 同价。修复：时间预算默认开（cesium 用 8ms）+ 按字节/像素加权，计数为兜底。
- ✅[已修复 2026-07-05] **P1-10 上采样地形瓦片走主线程同步 prepare**（GltfRenderResourcePreparer.cpp:121-593；TilesetContentLifecycleCoordinator.h:197-202 显式回落 sync）：上采样子瓦片携带父级过期 terrainGpuVertexBytes，上传编排的尺寸校验不过→回落主线程同步 prepare（顶点全量重建+无预算 GPU 上传），叠加 P1-9 深缩放一帧可串多次。修复：clip 完成时（TileGltfTerrainUpsampledChildMaterializer）按 loader decode 同一公式为子瓦片重建预置字节→自然走 GpuUploadQueue 异步路（budget lane 节流），顺带消除"父级残留字节尺寸巧合匹配→上传父几何"隐患。真机 2000m 深缩放验证：zoom 15（全上采样），51 attachments 全 exact、0 missing/kicked、terrain pending 0、59.6 FPS；30000m 与基线像素级一致。注：clip 本身仍在主线程 TileLoadScheduler 里执行（审计设想的"工作线程 clip"并不存在），如 profile 证明 clip 段有感再做 worker 化。RGB→RGBA 转码仅剩非地形 glTF 路径使用，不在地形热路径。

---

## 3. P2 发现（确认，单项中小、加总可观）

| # | 发现 | 位置 | 修复方向 |
|---|---|---|---|
| ✅P2-1 | curl 无 `CURLOPT_ACCEPT_ENCODING` → 无 gzip（QM/json 压缩比 2-5×；iOS NSURLSession 默认有 → 双平台行为不一致） | CurlMultiRequestScheduler.cpp:383-429 | 一行 setopt |
| P2-2 | sync-over-async：httpGet/getBlocking 在 AsyncSystem 池线程 cv 等网络（20ms 周期轮询），同池混装解码+磁盘写+网络等待 | GltfContentProvider.cpp:3358/3949/3993、QuantizedMeshLayerJsonFetcher.cpp:64-116 | 改 continuation 链或分池 |
| ✅P2-3 | 响应 body 无 reserve：75KB 按 16KB 块 ~8-10 次 realloc 在唯一网络线程 | CurlMultiRequestScheduler.cpp:22-28 | 按 Content-Length reserve |
| P2-4 | iOS/Mac 桥丢优先级（Mac 连 headers 一起丢）；并发 6 vs curl 20 不一致；NSURLCache 与 HttpCache 双份缓存 | IosPlatformBridge.mm:55-109、MacPlatformBridge.mm:57-60 | 映射 priority/headers、统一并发、禁 NSURLCache |
| ✅P2-5 | QM 共享 metadata 完成后按 waiter 逐个整拷 75KB body | QuantizedMeshTerrainProvider.cpp:~3965 | shared_ptr 分发 |
| P2-6 | 优先级为提交时快照：Low 预取可占满 20 槽、High 无抢占、无 updatePriority 通道（cesium 每帧重排做不到） | CurlMultiRequestScheduler.cpp:295-306 | High 保留配额或端口加 updatePriority |
| P2-7 | 主线程 pending 队列 O(N²) 优先级选取且持 lifecycle 锁扫描（积压数百时每帧数万次比较，阻塞工作线程投递） | TilePendingLoadQueue.cpp:145-192 | 堆/多级 FIFO |
| ✅P2-8 | trimUnusedTiles 每帧 O(缓存瓦片×待上传) 字符串比较 + 每瓦片一次 mutex 往返（300×50=1.5万次/帧） | RasterOverlayTileProvider.cpp:3719-3748 | 快照 key 到 set 再无锁遍历 |
| P2-9 | 字符串瓦片注册表 key：每次 ensureTile 现场拼接（50 entries ≈ 每帧 500+ 次构造 / 2000+ 次分配） | TileCacheKey.h:10-13、TilesetTileRegistry.h:17 | z/x/y 位打包整数 key |
| ✅P2-10 | 每帧无条件刷新 credits/progress，含 O(N²) 字符串去重 | TileRenderPlanFrameRefresher.cpp:80-178 | revision 门控 + unordered_set |
| ✅P2-11 | 遮挡检查每瓦片重复 cartesianToCartographic(相机)（迭代法测地转换，相机帧内不变） | TileSoftwareOcclusionPolicy.cpp:168-169 | 帧级相机快照传入 |
| P2-12 | 异步管线纹理按 primitive 重复拷贝，仅 primitives[0] 被消费（(P−1)×T 份全图浪费） | GltfRenderResourcePreparer.cpp:655-696、:905-907 | 拷贝提升到 GpuReadyData 层 |
| ✅P2-13 | createTexture 每次 glGetString(GL_EXTENSIONS) 全串拷贝 + glGetFloatv（均为驱动线程同步点），**每张 raster 瓦片纹理触发** | RenderDeviceGLES.cpp:23-27、:188-196（触发方 RenderDeviceRasterTextureUploader.cpp:97） | surface 创建时查一次缓存 |
| ✅P2-14 | updateBuffer/updateTextureRegion 每次 glGetError（threaded GL dispatch 下强制驱动线程同步；动画每 primitive 每帧一次） | RenderDeviceGLES.cpp:254、:291 | 仅 debug 保留 |
| P2-15 | Metal sampler 每纹理新建无去重（Apple GPU 存活 sampler 上限 1024/2048，逼近时静默回落掩盖问题） | RenderDeviceMetal.mm:197-208 | descriptor 缓存表 |
| ✅P2-16 | `framebufferOnly=NO` 无正当用途，放弃 drawable 压缩/scanout 优化 | MetalView.mm:30 | 改 YES |
| ✅P2-17 | CVDisplayLink→dispatch_async 无合帧：慢帧时主队列 render block 无限积压（输入延迟持续增长） | MetalView.mm:102-110 | pending 标记合帧 |
| P2-18 | 每瓦片纹理 create/destroy 无池化、glGenerateMipmap 在渲染线程；接口无 PBO/staging 异步上传表达能力 | RenderDeviceGLES.cpp:146-220、RenderDeviceMetal.mm:189-236 | 纹理池 + staging→private blit |
| P2-19 | submit 尾部每帧全量状态拆除（~40-50 GL 调用），状态影子为局部变量不跨帧 | RenderDeviceGLES.cpp:934-992 | 影子提升为设备成员 |
| ◐P2-20 | 每帧空 addCompletedHandler + CFRetain/associated object 存 drawable 绕远；pipeline/depth/cull 状态无去重 | RenderDeviceMetal.mm:563-591、:870 | 删空 handler、Impl 成员、状态记忆 |
| P2-21 | 每渲染瓦片每帧全量跑 overlay 映射状态机 + processingOrder vector 分配，无"未变化跳过"门控 | TileRenderCommandPreparer.h:55-85、RenderContentRasterOverlayStateUpdater.cpp:17-120 | revision 短路 |
| ✅P2-22 | onEnterBackground 在 UI 线程内联执行全部排队回调 | CurlMultiRequestScheduler.cpp:201-217 | 转池执行 |

---

## 4. 疑似项（机制确认 / 幅度需 profile）

- 所有毫秒估计（GL 调用排队、哈希/分配成本随设备浮动）——建议 Adreno 真机 perfetto/AGI 看 `submit` 段 + 驱动线程；Metal 用 Instruments Metal System Trace。
- `validateMvpRenderCommands` release 是否保留（RenderCommand.cpp:153-283，每命令 3 次 find）。
- 存在任一 blend 命令即全列表 stable_sort（SceneRenderPipeline.cpp:356-358，RenderCommand >500B）。
- collectInactiveTiles 每帧 O(全注册表) + 拷贝 cacheKey（TileFrameState.cpp）。
- glBufferSubData ghost-vs-stall 具体行为依赖驱动；Apple Silicon 上 shared 存储采样惩罚多半可忽略（独显 Mac 不可）。
- Accept-Encoding 收益在局域网地形服务器下小，公网源显著。

## 5. 正确性顺带发现（非性能，仅提示不动手）

- `RenderCommand.h:70-71` 的 `blendSrc/blendDst` 被 GLES 后端完全忽略，硬编码 alpha 混合（加法混合会错）。
- Metal 无 mipmap 生成 → 三线性采样读未初始化 mip 数据（P1-6 的正确性面）。
- Android `detachJni()` 无条件 Detach 可摘下本已 attached 的 Java 线程（P1-8 的正确性面）。
- `renderer/TileTextureCache.{h,cpp}` 全仓 0 引用，死代码。
- RasterOverlayTileProvider.cpp:3610-3612 每次上传无条件 Info 日志。

---

## 6. 修复优先序建议（按 AI 协作工时 + 杠杆排序）

1. **一行修三连**（合计 <1h，立即可做）：`curl_multi_wait`→`curl_multi_poll`（P0-3）；`CURLOPT_ACCEPT_ENCODING`（P2-1）；`framebufferOnly=YES`（P2-16）+ 删空 completedHandler（P2-20）+ anisotropy 查询缓存（P2-13）+ 热路径 glGetError 移除（P2-14）。
2. **uniform 句柄化 / POD block**（P0-1+P0-2+P1-2+P1-3，约 1-2 天 AI 协作）：一次改动消掉上游 5 万分配/帧、GLES 6000 调用/帧、Metal 绑定风暴与 31-buffer 死胡同。SurfaceTile 定长块是现成模板。做完后 GLES submit GL 调用预计 ~10k→~1-1.5k/帧。
3. ✅ **StreamingSet 真缓存化**（P0-4，半天-1 天）：内容未变只更 frameId + per-frame uniform；依赖 2 的结构化 uniform 更顺。→ 已修（见 §1 P0-4）。
4. **VAO**（P1-1，半天）：与 2 正交可并行。
5. **上传策略时间闸门默认开 + 字节加权**（P1-9，半天）+ 上采样瓦片异步化（P1-10，1 天）。
6. **HttpCache shared_ptr 化 + 移出网络线程**（P1-7，半天）；Android JNI 解码缓存/native 化（P1-8，半天-1 天）。
7. Metal in-flight 信号量 + drawable 延后 + mipmap blit（P1-4/5/6，1 天）。
8. P2 长尾按接触到的文件顺带成批处理（每项多为局部改动）。

**验证标准**（目标驱动）：真机 Adreno 730 上抓 perfetto，`Renderer submit` + 命令构建段 CPU 时间改前/改后对比；目标 submit 段 ≥50% 下降、malloc 计数（malloc_debug）每帧 ≥80% 下降；Metal 侧 Instruments 看 encode 时间与 drawable 等待。功能回归：139/139 native 测试 + selector golden 对拍 + 真机截图对比。
