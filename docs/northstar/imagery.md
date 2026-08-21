# 影像模块北极星 — 产品体验判据

**这份文档回答「做到什么程度算好、现在到哪了」。**
不回答「代码在哪」(那是 `AI_INDEX.md`),也不回答「当时怎么修的」(那是 `docs/issues/*`)。
`docs/issues/` 是写完即冻结的历史档案;**本文是活的**,随每次专项收官更新。

用法与 `vector.md` 同:判据编号 `I-V*` / 性能债编号 `I-P*` 是**跨会话稳定引用锚点**
(只增不改);状态 ✅ 达成 / ⚠️ 有缺口 / ❌ 未做 / 🔒 待你拍板;
【机制】类我自证(命令输出、测试红绿),【观感】类**像素判断归你**,我只钉复现场景 + 给截图;
「代价」列没量化过就写「未量化」,**不许填"应该很小"**。

> 本文创建于 2026-08-15,起因是「祖先-only 空洞」专项收官。
> 此前影像模块无北极星文档,历史判据是**从代码与测试倒推的**,凡带 `推断` 标记
> 的条目都不是你说过的,优先请你校对——校错了比没写更糟。
>
> **编号迁移(2026-08-19)**:本文原用裸 `V*`/`P*`,与 `vector.md` 的 `V*`/`P*` 跨模块
> 撞车(两个活跃模块都有「V11」)。按 CLAUDE.md 前缀约定统一改为 `I-V*`/`I-P*`。
> 一次性重映:旧 `V1..V12`→`I-V1..I-V12`、`P1..P6`→`I-P1..I-P6`(编号数字不变,只加
> `I-` 前缀)。历史对话/记忆里出现的裸 `V11` 等,在影像语境下即指对应 `I-V11`。

---

## 北极星一句话 `推断`

> 影像数据缺失时,屏幕上**永远不出现空白**:退化路径是「更低分辨率的真实影像」,
> 而不是「什么都不画」。且这种退化**不额外花钱**——复用祖先已有纹理 + UV 窗口,
> 不为一块空洞多传一张纹理、多占一份显存。

对照系不取某一具体实现,取**用户预期**:主流地图在数据洞上给模糊的父级影像,
用户读到的是"这里精度不够",而不是"这里坏了"。空白传达的是后者。

---

## A. 缺失与退化(本次专项)

| # | 判据 | 类型 | 状态 | 代价 | 证据 |
|---|---|---|---|---|---|
| **I-V1** | 目标层级源瓦全缺时,该瓦显示**祖先影像**而非空白 | 【观感】 | ⚠️ 机制已通,像素未判 | 0(复用祖先纹理 + UV 窗口,`uploadCount == 0`) | 见下方「I-V1/I-V2 机制证据」 |
| **I-V2** | 一块空洞瓦**不得毒化其子孙链** —— 子孙应越过它取到更高层可画祖先 | 【机制】 | ✅ 达成 | 同上 | 同下 |
| **I-V3** | 空洞瓦不驱动细化(refine 进洞只会得到更多洞) | 【机制】 | ✅ 达成 | 0 | `originalFailed_` 置位 → `MoreDetail::No` |
| **I-V4** | 退化不得引入逐帧重合成 / 重下载 | 【机制】 | ✅ 达成 `推断` | 0 网络 | 空瓦按 cacheKey 命中既有 tile;`loadMappedRasterTile` 对 Loaded 早退。**未做长时真机计数验证** |
| **I-V5** | 空洞区的边界不应出现"半张图"错缝 | 【观感】 | ❌ 未验 | — | 无复现场景,待你给 |

### I-V1/I-V2 机制证据(2026-08-15)

根因不在 compose:祖先-only 时 `composeMappedSourceImageSet` 刻意返回 0×0 空图
是**对的且最便宜**的设计(避免把父像素烘成新纹理),坏的是**交接**——

1. 空图经 `markLoadedWithoutTexture()` 落成 `Loaded`,被 Step 3 当正常瓦提升为
   ready 并清空 `_pLoadingTile`,而 Step 4 的祖先替换只在 `_pLoadingTile != nullptr`
   时才跑 → 该瓦**永久空白且不可恢复**(吸收态)。
2. `findLoadedTileOverlay` 只看 `Loaded/Done` 不看能否渲染,父链遍历撞上空瓦即
   `break` → 明明祖父有纹理,**整条子孙链都不画**。

修法:`RasterOverlayTile` 加显式 `isEmptyComposition()` 标记(**不能用「Loaded 且
无纹理」推断** —— `setState()` 是通用 setter,测试夹具会造出无纹理的 Loaded 瓦,
第一版用无纹理当判据误伤了 9 条既有测试);Step 2 的回退触发条件从"仅 Failed"
扩为"Failed **或** 空合成",`findLoadedTileOverlay` 跳过空合成候选。

- 测试:`BlankAncestorOnlyRasterFallsThroughToDrawableGrandparentForSelfAndChild`
  (由 `TemporaryAncestorFallbackAcceptsNoTextureParentBeforeDrawableGrandparent`
  改写而来 —— 旧测试**把毒化行为当契约钉死了**)
- A/B:`git stash` 掉源码改动后该测试红(父与子都停在空瓦、`attachCount == 0`、
  binding `None`),恢复后绿。全量 `ctest` 187/187。

> **I-V1 仍是 ⚠️ 而非 ✅**:机制我自证了,但"空洞区显示模糊父级影像"好不好看是
> 【观感】判定,归你。需要你给一个能稳定复现空洞的场景(哪个源、哪片区域、
> 哪个层级),我出截图。

---

## B. 缓存与网络

| # | 判据 | 类型 | 状态 | 代价 | 证据 |
|---|---|---|---|---|---|
| **I-V6** | 影像瓦走共享 HTTP 缓存,内存缓存逐出后回摇不重下 | 【机制】 | ✅ 达成 | — | commit `7f86e76b5`(I-P1 结清);WMS/WMTS/Bing/Google/TMS 均继承 `XYZImageryProvider` 的缓存路径 |
| **I-V7** | 缓存条目可过期 / 可重验(ETag、304),陈旧影像不会永久钉死 | 【机制】 | ❌ 未做 | — | `HttpCache::expiryTime` 全仓**只有定义没有生产赋值**,见 I-P1 |
| **I-V8** | 失败瓦不被反复请求 | 【机制】 | ✅ 达成 | 每条失败记忆 1 字节配额 | `cacheTerminalFailure` 写 `terminalFailure` 条目,查找侧当命中短路 |

---

## C. 性能债(明记,不假装没有)

| # | 债 | 影响 | 状态 |
|---|---|---|---|
| **I-P1** | `HttpCache` 无过期 / 无 ETag 重验 | 影像一旦缓存永不刷新;源端更新用户看不到 | 未做。**TTL 口径是跨会话共享约定,须你先拍**再实现 |
| **I-P2** | `FrameResourceBudget::canIssue` 忽略 `FrameResourcePriority` | 预算紧张的那一帧,urgent 可能被 preload 占满名额而 Blocked | 未做。影响被 HTTP 层动态优先级 cell 部分兜底(下一帧重试时排在前面),**是帧级延迟不是持续饥饿** |
| **I-P3** | 空洞瓦每帧重走完整 `update()`(state 非 `Attached`,`hasStableUpdateState()` false) | 洞区瓦片的 per-frame CPU;无网络无合成 | ✅ 已量化(2026-08-22 host,Debug/Release 双口径):机制属实(空合成瓦 ready 无 rendererResources → Step 1 每帧踢回 Unattached),但成本可忽略——Release 每瓦每帧 update() 差值 0.00002ms、prefetch() 差值 0.00019ms(200 洞瓦 ≈ 0.04ms/帧)。**不立项**。证据:`test_raster_hole_update_cost` 2 测试 |
| **I-P4** | `RasterOverlayTileProvider.cpp` 4785 行,四路失效逻辑互调 | 可维护性,不是运行期成本 | ✅ 已拆完(2026-08-22 第三刀):主文件 4785→4171(投影+合成簇)→3122(depot 嵌套 struct)→**2829**(状态预算簿记 `RasterOverlayTileProviderBudget.cpp`)。每刀逐字搬移(第二刀起用 `git show HEAD | sed` 提取,一次全绿);互调解耦仍靠前序两工具函数。raster ctest + 全量 199/199 绿。**依赖闭包已确认(2026-08-22 分析,见下)** |

**I-P4 第四刀设计(2026-08-22,依赖闭包分析结论,未实施)**:
匿名 helper 已按簇分层,每刀需求:
- **收口刀 4a**:`getTile/resolveTile` 与 `loadMapped/pump/sourceTileList` 共享的 6 个纯几何 helper(`effectiveCoverageRectangle`/`mapGeometryBoundsToImageryCoverage`/`shouldClampOutsideCoverage`/`schemeCoverageRectangle`/`expandClampedLineIntoCoverage`/`inwardSampleEpsilon`)→ 先收口进 `RasterOverlayImageCompositing`(零 provider 状态,契合其既有定位),主文件删本地定义。
- **拆刀 4b**:`loadMappedRasterTile`..`issueActiveMappedSourceImageSets`(约 630 行)→ 新 TU `RasterOverlayTileLoadDispatch.cpp`,自带 5 个专属 helper(`logAndroidRasterPipeline`/`availableRasterRequestSlots`/`hasRasterInflightCapacity`/`gNextRasterSourceWaiterOwnerToken`/`isTransientRasterSourceFailure`)+ 复用收口后的共享集。
- **拆刀 5**:`processPendingUploads`+`hasPendingWork`(约 350 行)→ 新 TU,自带 4 个专属常量/helper(`kDefaultMaximumRasterUploadsPerFrame`/`uploadAllowedDuringInteraction`/`kInteractionRasterUploadMaxDimension`/`kInteractionRasterUploadMaxPixels`),闭包最干净。
- **拆刀 6(可选)**:`syncWorkTickets`/`syncRasterLandingTicket*`/`trimUnusedTiles`(约 160 行),只需 `isEpochMappedRasterCacheKey`+`kRetainedUnusedFrames` 两个 helper。
- 每刀沿用「`git show HEAD | sed` 逐字提取 → 新 TU 自测 + raster ctest + 全量 + AI_INDEX 行号同步」流程。拆完主文件约 1700-1900 行。

---

## D. 合规

| # | 判据 | 状态 | 证据 |
|---|---|---|---|
| **I-V9** | Google Maps Tiles ToS 要求的归属 logo 显示 | ❌ 未做 | `GoogleMapTilesImageryProvider.cpp` 的 TODO;曾有的 `showLogo` 占位位(恒 true、无消费者)已于 2026-08-07 删除 |
| **I-V10** | WMS 版本适配:`version` 可配但只实现了 1.3.0 口径 | ✅ 达成(2026-08-22) | buildUrl 按 version 前缀切参数名:1.3.x → `crs`,其他(1.0/1.1.x)→ `srs`;BBOX 保持 lat,lon(south,west,north,east,与 cesium 一致)。新增 `test_web_map_service_imagery_provider` 4 测试钉死 1.1.1/1.3.0/1.0.0/默认版本 URL |

---

## G. 坐标系与配准(GCJ-02,2026-08-16)

| # | 判据 | 类型 | 状态 | 代价 | 证据 |
|---|---|---|---|---|---|
| **I-V11** | 缩放过渡期(祖先回退/模板态)影像不错位 —— 低空拉高空的过渡瞬间,GCJ 底图瓦片无可见跳变 | 【观感】 | ✅ 达成(2026-08-16 真机用户判定) | 过渡帧 FS 每 mappedRaster 层 +1 分支(未量化,分支恒定无发散) | 真根因=mappedRaster **回落路**漏 remap(aa99a4ac5 只修了 pageStore 路),`926271866` 补齐;稳态主路=四角仿射(`80892aca1`);数值上界 `test_computed_imagery_uv_risk`(`e5126d617`+`a9d5289ca`)。**验收场景**:重庆 106.508/29.617 GCJ 底图,camH 1500→50km 过渡瞬间,用户判定通过 |
| **I-V12** | 跨中国框瓦片的配准退化**有界且被闸住**:δ 阶跃仿射原理性代表不了,但不得击穿合批资格闸(纯白面) | 【机制】 | ⚠️ 白面闸已修,退化处置待拍板 | 跨界瓦片(仅中国框四边)放弃视锥剔除,多取几张页 | 白面闸=`f06117978`;退化上界=中心拟合 4.52px@z10(境外侧,risk test 打印);🔒 接受 vs 挡回烘焙路径,归你 |

**边界诚实**:I-V11 的机制对**标准 mercator 底图**同样生效(此 bug 非 GCJ 特有,
δ 只是把它放大到肉眼可见);z≤5 的过渡态残差(模板 UV 纬度线性 vs merc 参数化,
数 px 级,标准底图同样命中)未修,见 I-P5。中国框四常数(72.004/137.8347/0.8293/
55.8271)是开源惯例值,未与高德实际偏移域比对过——若不符,I-V12 的"有界"位置
整体平移。

### 性能债补记(接 C 表)

| # | 债 | 影响 | 状态 |
|---|---|---|---|
| **I-P5** | 模板 UV 纬度线性 vs merc 参数化:z≤5 过渡态影像竖向残差(0.284·半跨·256px,z5 约 7px;标准底图同样命中) | 全球尺度缩放过渡的短暂错位;稳态自愈 | 未做。修法候选=粗瓦不走模板路径;待真机确认可见性再立项 |
| **I-P6** | fill 代理 UV 仍用地形 scheme 投影(GCJ 下二阶误差,米级,aa99a4ac5 遗留) | fill 兜底面上的影像微偏 | ✅ 已量化(2026-08-22),**不立项**。境内典型城市全 zoom 亚像素:重庆最坏 0.88px/4.2m @ z15(256px 瓦口径);全国粗扫最坏 1.20px @ (105,49) z17、高纬精扫 1.29px @ (106,53) z18、米级最坏 215m @ z6;跨框瓦 380m/2.46px(境外侧,并入 I-V12 开放裁决)。fill 是临时视觉桥、显示期短,修法留档=fill 模型按 overlay 投影逐顶点 GCJ texcoord。证据:`test_fill_proxy_uv_risk` 4 测试 |

---

## E. 已判死 / 勿再提(边界)

- **不要让 compose 把父像素烘进新纹理来填洞。** 那是最贵的一条路(多一次上传 +
  多一份显存 + 多一次采样链),而祖先纹理 + UV 窗口能拿到同样的像素、零额外成本。
  空图返回是**刻意设计**,坏过的只是交接,已修。
- **不要用「`Loaded` 且无纹理」判定空合成。** `setState()` 是通用 setter,
  会误伤正常构造的瓦(实测误伤 `test_tile_child_materializer` 2 条 +
  `test_sse_pipeline` 7 条)。判空合成只认 `isEmptyComposition()` 显式标记。

---

## F. 待你拍板的开放项

| 项 | 我的建议 | 锚点 |
|---|---|---|
| 空洞区显示模糊父级影像 vs 干净空白 | 我押父级影像(用户读作"精度不够"而非"坏了"),但这是像素判断,归你 | I-V1 |
| `HttpCache` 过期口径(固定 TTL?尊重源端 Cache-Control?) | 建议先做 ETag/304 重验,TTL 次之 —— 重验不猜时长 | I-V7 / I-P1 |
| WMS 1.1.1 是否要支持 | 我押支持(纯逻辑缺口,可单测钉死,改动面小) | I-V10 |
| Google 归属 logo 是否要做 | 取决于你是否真用 Google 源发布 | I-V9 |
