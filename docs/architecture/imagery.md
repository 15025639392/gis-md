# 架构沉淀:影像 / 栅格叠加与数据源 (imagery + providers)

> **架构/方案**文档。质量标尺看 `docs/northstar/imagery.md`(判据 I-V*/债);行号看 `AI_INDEX.md` §7/§9/§15;行号随重构漂移,以符号名为准。北极星里标"推断/未验证"的结论本文原样保留标注,不加固成"已验证"。

**规模**:`providers/` 11.5k 行。

---

## 职责边界

`providers/` 是**只产出 CPU 解码影像**的数据源层,不碰 GPU:`ImageryProvider`(头注释明写 "owns no GPU resources")只做「建 URL → HTTP fetch → decode 成 `DecodedImage`」,`TerrainProvider` 同构但产出 `DecodedHeightmap`。GPU 上传边界显式切在 `RasterTextureUploader` 接口——provider 侧永远停在 CPU 图像。

往上的职责链:
- `RasterOverlay`(`layers/`)——纯配置:持 provider + scheme + Options,不跑运行期状态。
- `ActivatedRasterOverlay`——运行期壳,懒建真实 `RasterOverlayTileProvider`(+ `RenderDeviceRasterTextureUploader`),construction 期先给 null-uploader 的 `placeholderProvider_`。
- `RasterOverlayTileProvider`(4785 行)——对齐 cesium `RasterOverlayTileProvider`:栅格瓦缓存、异步加载调度、GPU 上传排程、节流、source-tile depot、按帧 trim。
- `DirectRasterMapping`(`tiling/`)——对齐 cesium `RasterMappedTo3DTile`:把「一张地形几何瓦」和「一张影像瓦」用三态挂接机(Unattached / TemporarilyAttached / Attached)绑一起。

**不含**矢量渲染——但 `VectorDrapeImageryProvider` 是例外:它把矢量面数据**伪装成影像 provider**接入这同一条通路(见扩展点)。

### Overlay Runtime 迁移边界（2026-08-28，统一 Resolution + GPU serial 阶段）

每个 `Tileset` 现在持有一个 `RasterOverlayRuntime`，由它统一托管：

```text
RasterOverlay 配置 / ActivatedRasterOverlay 顺序
  → RasterOverlayRuntime
       ├─ RasterOverlayFrameContext（每帧冻结 slot / provider view / generation）
       │    ├─ Direct view（保留原 slot，过滤项为 nullptr）
       │    └─ PageStore provider view（canonical-order 紧凑 provider 列表）
       ├─ RasterBindingSet（按帧解析 backend-neutral RasterResolution）
       │    └─ DirectRasterSampleDescriptor（Direct 专属 texture/UV/lease）
       ├─ RasterOverlaySourcePlan（纯 geometry→source LOD / coverage / keys）
       └─ RasterAssetDepot（backend-neutral decoded source acquire + lease）
             → RasterOverlayTileProvider source depot
                  cache + in-flight + waiter lease + transport + decoded image
```

- `RasterOverlayBackend` 仍是 provider-view 策略接口；Runtime 按 `Direct` / `PageStore` 持有实现，启停和替换不改变 Scene/SDK 的 overlay 配置所有权与顺序。Runtime 会把 backend 返回的 provider 重新规范为配置顺序的子集，禁止重复、乱序或外部 provider 改变 runtime slot。
- `RasterOverlayFrameContext` 在 Tileset 每帧所有 content request、selection、mapping、upload 和 render-plan 工作开始前冻结。Direct view 的 vector 长度永远等于 Runtime overlay 数；backend 排除的层用 `nullptr` 占位，而不是压紧下标。这样 `TileRasterOverlayState::mappings_[i]`、projection texcoord、样式与最终绑定始终使用同一个 runtime slot。
- 迁移期的 `Tileset::rasterOverlays_` 引用 alias、`RasterOverlayRuntime::ensureProviders()`、可变 `overlays()` 和 Runtime 顶层 Depot handle 已删除。Direct 的 selection/request/upload/readiness/draw 统一从 `frameContext().directOverlays()` 取值；Scene 收敛、diagnostics 与 cache accounting 则显式读取 `configuredOverlays()`，不再混用“配置栈”和“本帧 backend view”。backend provider 解析只在 Runtime 内部用于发布下一帧快照，执行链无法再绕过帧上下文。
- Direct/PageStore 分别有 generation。backend 替换、启停或 provider view 变化只在下一次 `beginFrame()` 生效；Direct generation 变化会先释放 tileset 上的旧 mapping，并清理 provider 的 Direct mapped/direct tile、在途 mapping owner 与待上传状态。backend-neutral exact source cache 不随 Direct 失效清除，PageStore 已持有的 decoded source 也不会被误杀。
- `RasterOverlayFrameSlot` 已按值冻结 `visible`、`opacity`、`role`、`priority`、`fallbackPolicy`、`blocksCompleteRenderable`、projection、provider revision 与 generation。Direct binding 和 PageStore source publication 都消费该帧值；`beginFrame()` 之后修改 overlay 配置只会在下一帧发布，不会让同一帧的 readiness、draw 与 page bake 读到不同值。
- `RasterResolution` 把目标请求态（NoMapping/Placeholder/Unloaded/Loading/Loaded/Failed/Empty）、当前内容形态（None/Exact/Composed）、显示来源（Own/Ancestor）、attachment、coverage、target failure/pending、desired/resolved zoom 与帧元数据正交表达。失败目标与可画祖先可以同时存在，不能再压成单一 “mapped/not-mapped” 状态。
- `RasterBindingSet` 按 runtime slot 发布 `RasterResolution`，readiness 和 draw command 共用同一语义 resolver。draw command 已不再读取 `DirectRasterMapping*`；Direct 的 `Texture*`、texcoord、UV affine 与资源保活被隔离在 `DirectRasterSampleDescriptor`，避免把 PageStore 的 array + indirection + placement 伪装成单纹理合同。
- `RasterOverlaySourcePlan` 已从 Direct composite 专属实现中抽出：它以值对象表达 source zoom/bounds/keys/range 与 exact-single-source 判定，纯函数统一执行 MSE LOD、最大纹理尺寸降级、反经线拆分和 Cesium `1/512` 边界容差。Direct geometry mapping 与 Direct load 共同消费该合同；规划层不持有 provider cache、request、upload、attachment 或 GPU uploader 指针，因此 PageStore/未来 backend 可以共享它。
- `RasterAssetDepot` 是 Tileset Runtime 持有的统一访问边界。PageStore 不再直接调用 `ImageryProvider::requestTile()`，而是通过 exact-only acquire 加入 provider source depot；Direct composite 仍使用同一个 provider source depot，因此同 key 只产生一次 transport / decode。
- cache hit 和 join in-flight 不消耗新的 Scene network grant；consumer lease 取消只摘自己的 waiter，不取消其他 backend 正在共享的请求。
- `RasterAssetKey` 包含 provider instance、content revision、depot epoch、scheme、projection 和 source key，防止换源、换样式、mixed-scheme 或 provider 重建后串图。
- PageStore 只接 exact source，不继承 Direct 的祖先回退；source 失败或 malformed 时 PageStore 保持 miss，继续显示 Direct fallback。共享在途请求只代表一次 exact transport，失败后的 resolution policy 按 waiter 独立执行：ExactOnly 在子瓦失败处结束，Direct waiter 仍可进入父级链；因此两者的结果不再依赖谁先发起请求。切换 Direct backend 会清理当前 Direct mapping/direct tile 与 pending upload；这是安全的代际隔离，但即使新旧 provider 集合逻辑等价，也可能造成重新请求/上传 churn，当前没有端到端请求数 A/B 证据。
- PageStore 从 Depot 获得的 decoded image 是 accounted aliasing lease。即使 source cache 已淘汰，只要 compose/worker 仍持有 image，其物理 CPU 字节仍计入 `externalPinnedSourceBytes`；`getSourceDepotResidentBytes()` 对 cache、pending upload 和多个 external lease 按 `DecodedImage*` 去重，只算一次实际驻留。source cache 再填充也会先扣除 external-only residency，避免“缓存已驱逐但外部仍持有 A”时又缓存 B 而越过同一 `subTileCacheBytes` 物理预算。

当前迁移已清除旧 overlay 容器 alias 和公开绕行入口，完成帧元数据值快照、backend-neutral SourcePlan、共享 Asset Depot、`RasterResolution` / `RasterStackResolution` 以及 backend-specific sample descriptor 隔离。PageStore 的 `RasterStackBinding` 正式由两部分组成：中立的 stack presentation/fallback 状态，以及 `RasterStackSampleDescriptor` 中的 image/field array、indirection array、layer、texcoord、cell zoom、placement 和 geometry affine。Direct 与 PageStore 可以比较“是否可画、覆盖是否完整、是否仍需回退”，但不伪装成同一种 GPU 纹理形态。

PageStore 同时具备两层生命周期保护：`SubmissionLease` 在 CPU 命令构建到 submit/reference release 期间冻结 mutation；`submittedSerial/completedSerial` 则追踪真实 GPU 使用边界。array、indirection 和 field layer 在 pending use 或 completion frontier 尚未越过 `lastUseSerial` 时不得原位覆写；rebake/replace 使用 COW replacement 或 staging key，上传完整成功后才 `replaceKey()` 发布。domain reset 与 eviction 只撤销逻辑映射，旧物理层延迟到 completion 后回收；部分 indirection upload 失败时 staging 不会替换已发布页。

仍保留的正式边界是：decoded cache/in-flight 的物理状态继续按 provider endpoint 分区；Runtime Depot 是统一访问 facade，不是一份跨 provider 的物理资产哈希仓；Direct 的 loading/ready 双槽、祖先回退、attachment 与 composed upload executor 仍位于既有 tiling 链。旧公共 `mappedRaster` 身份、shader ABI 与诊断名称已经退场，但 Direct mapping/composite 是 PageStore miss/failure/eviction/admission、mixed-scheme、非真实地形和祖先回退所需的正式 backend 能力，不能删除。

### 通用性、叠加层与 MVT 配合边界

- 普通 raster overlay、WMS/WMTS/TMS/XYZ、GCJ adapter 和“矢量先栅格化”的 MVT 面 drape 都可以复用 Runtime + Depot；关键条件是 provider 最终产出带明确 scheme/projection 的 `DecodedImage`。
- MVT 面接入点是 `VectorDrapeImageryProvider` 一类 adapter：MVT 获取/解码仍属于矢量 source，adapter 把指定面层栅格化成 canonical page image，再参与 PageStore 或 Direct composite。PageStore 不直接理解 MVT extent、source-layer 或原生瓦片坐标。
- MVT 点/标注/几何线继续走 Scene 托管的 `MvtVectorSource → FeatureRenderLayer`，不应塞入 raster overlay runtime；两条系统只在 Scene 帧级资源仲裁和最终地形/深度语义上协作。
- 历史 `RoadFieldSource → LineFieldRasterizer → TerrainPageStore road-field plane` 已完整物理删除，不再是 Overlay Runtime、Asset Depot 或 PageStore backend。MVT 点/标注和 `FeatureRenderLayer` 几何线不受影响。

“最多 4 overlay”只限制 Direct/glTF draw command 的**同时采样槽位**：`RenderCommand` 固定有 `kMaxGltfRasterOverlays == 4` 组纹理、UV scale-bias、texcoord 与 opacity。Runtime/SDK 可以配置更多层，但当前 PageStore 必须保持与 miss 时 Direct fallback 完全同源；因此 active source 超过 4 个时，PageStore 对该有序栈整组 fail-closed，不能先合成 5+ 层再让 Direct miss 只画前 4 层。它不是 Runtime 最多管理 4 层，也不是 PageStore 技术上只能合成 4 层，而是现阶段的跨 backend parity 闸。

PageStore 的 canonical stack 还有一个像素级前提：首个 active source 必须是 `BaseImagery` 且配置 opacity≈1，annotation-only 或半透明 base 整组 fail-closed；但配置不透明并不能证明 PNG 每个像素 alpha 都为 1。canonical base 若自身含透明洞，PageStore 页仍可能让下层 Direct 栈透出并发生重复混合。在增加 guaranteed-opaque 元数据或 lower/Page/upper 分段合成前，这仍是明确的接入约束。

---

## 核心设计决策 + 理由

### 1. raster overlay → tileset tile 的映射
入口 `RasterOverlayTileProvider::mapRasterTilesToGeometryTile`(对齐 cesium `QuadtreeRasterOverlayTileProvider`):
1. 几何矩形按 `projection_` 反投影回地理,与 `sourceCoverageRectangle_` 求交——不在覆盖内直接返回 `{nullptr,...}`(**空洞的第一道来源**)。
2. backend-neutral `buildRasterOverlaySourcePlan` 按 SSE/纹理尺寸算源 zoom + 源瓦矩形集，并给出 `exactSingleSource`。
3. **单源精确命中**直接返回原生瓦(`directTile=true`,当前兼容字段),不额外合成——最省的一条路。
4. 否则落"合成瓦"路径:以 `directCompositeTileCacheKey`(含 `directCompositeTileEpoch_`)查/建一张 `RasterOverlayTile`,后续异步拉多张源瓦调 `composeDirectCompositeSourceImageSet` 拼成一张纹理。

`DirectRasterMapping::update()` 是 7 步状态机:Attached 快路 → 失败/空合成回退父链 → 常规映射 → Loading→Ready 提升 → 祖先替身接管 → 失败无回退兜底 → attach → 报 `MoreDetailAvailable`。

### 2. 多协议 provider 抽象
`ImageryProvider` 是纯虚接口。真正承重的是 `XYZImageryProvider`——TMS/WMS/WMTS/BingMaps/GoogleMapTiles 全继承它,只覆写 `buildUrl`/`ctor`(WMS 甚至强制 `Geographic-TMS` scheme)。
- **收益**:HTTP 缓存、decode、节流只写一遍。
- **代价**:协议差异压缩成"占位符替换表",WMS 1.1.1 之类缺口只能在 buildUrl 里长 if。`DebugImageryProvider` 是唯一不走 XYZ 基类的实现(无网络棋盘格)。

### 3. GCJ-02 偏移坐标系怎么接(自研,cesium 无对应物)★
分两层,**刻意不糅合**:
- **坐标变换本体**(`core/geodesy/Gcj02CoordinateTransform`):`fromWgs84`/`toWgs84` 只对中国框内点生效(边界常量标注"开源惯例值,未与高德实际偏移域比对过")。`toWgs84` 不动点迭代 3 轮。`boundRectangleFromWgs84` 用**区间算术** + 8×8 网格逐格转换取并集,避免非线性变形导致矩形失真穿透。
- **接入投影层**(`RasterOverlayProjection` 三态 `Geographic / WebMercator / Gcj02WebMercator`):`projectWorldPositionForRasterOverlay` 对 WGS84 顶点先 `fromWgs84` 再 WebMercator;而**源矩形侧** `projectRasterSourceRectangle` 故意不再叠 GCJ——XYZ 源瓦矩形本身已是 GCJ-02。
- **核心不变量**:世界几何走 WGS84→GCJ 单向变换,源瓦坐标系本身就是目标系,不二次变换。

### 4. upsample 子瓦片协调
不单独实现——**复用祖先纹理 + UV 窗口**是唯一降级路径。子瓦走 `TileContentUpsampleKind::RasterDetail` 时,其 `RasterOverlayDetails` 矩形从父矩形几何裁剪得到,再喂 `mapRasterTilesToGeometryTile`——仍去要更细源瓦,只是不必重算投影 UV 起点。

### 5. per-tile 纹理窗口
`computeTranslationAndScale` 在 Step3/Step4 分别对"自己的真实瓦"和"祖先替身瓦"重算 offset/scale,`rasterUV = geometryUV * scale + offset`——祖先纹理不变,只是 UV 窗口收窄。

### 6. Direct composite 与 PageStore 的 mixed-scheme 分工

Direct composite 和 `TerrainPageStore` 不承担同一种 scheme 语义：

- Direct composite 以**单个 overlay provider**为单位，把 geometry rectangle 映射到该 provider 自己的 projection/tiling scheme；因此 geometry 与 overlay scheme 可以不同。这部分行为对齐 cesium-native。
- `TerrainPageStore` 以**一个有序 provider compose group**为单位，只维护一份 page placement、UV、fetch key 和 scheme-less `packKey(z/x/y)`。因此 group 内必须共享：

```text
PageStoreCompatibilityKey = {
  canonical page-facing TileScheme semantics,
  effective RasterOverlayProjection
}
```

具体规则：

1. `providers[0]` 是 canonical page domain；当前真实地形 scheme 必须与它相同。
2. 后续 provider 可有不同内容、最大 zoom 和源图尺寸，但必须能消费 canonical `PageKey` 并输出 canonical 页空间图像。
3. XYZ/TMS、Mercator/Geographic、Standard/GCJ 等不兼容组合整组退出 PageStore，继续走 Direct fallback；不得只丢弃冲突 source。
4. 原生 mixed-scheme 数据若要参与页合成，转换职责放在 adapter 内：adapter 自行枚举原生源瓦、重投影/重采样，再把结果伪装成 canonical imagery。`VectorDrapeImageryProvider` 是现有先例。AMap classic-normal 生产路径是纯矢量，不保留 AMap raster adapter。
5. domain 或 provider set 变化会推进 generation；所有晚到的 imagery/compose/field/upload item 必须同时匹配 generation、页 key 与 layer，不能仅凭相同 `z/x/y` 复用。

因此，“Cesium 支持 geometry/overlay scheme 不同”不能推导出“PageStore 支持多个异构 overlay 共页”。前者是 provider 独立映射，后者需要 per-source placement 与 page namespace，当前没有实现。

---

## 数据流(关键路径)

```
ImageryProvider::requestTile (HTTP/bridge)
  → XYZImageryProvider::tryServeFromHttpCache (命中则下沉 worker 解码)
  → decodeTile (stb_image → DecodedImage)
  → RasterOverlayTileProvider 源瓦异步汇入 ProviderAsyncState (SharedAssetDepot 语义)
  → composeQuadtreeSourceImagesWithDetails / composeDirectCompositeSourceImageSet
     (多张源瓦拼成目标矩形图像;祖先-only 场景刻意返回 0×0 空图——设计如此,非 bug)
  → PendingUpload 入队
  → processPendingUploads (主线程):
       emptyImage → RasterOverlayTile::markLoadedWithoutTexture() (置 emptyComposition_)
       非空 → RasterTextureUploader::uploadRasterTexture → setTexture (置 Loaded)
  → DirectRasterMapping::update() 7 步机
       Step2: overlayTileCannotRenderItself(Failed || isEmptyComposition) → 走父链
       Step3: Loading→Ready 提升, computeTranslationAndScale 定 UV
       Step4: 祖先替身接管(ancestor-only 空洞的兜底)
       Step6: attachReadyTileInMainThread
  → resolveDirectRasterBinding
       ├─ RasterResolution（请求/显示/attachment/zoom/帧策略）
       └─ DirectRasterSampleDescriptor（纹理/texcoord/UV/resource lease）
  → RasterBindingSet（按 runtime slot 发布给 readiness + draw）
  → 地形着色器采样上屏
```

---

## 关键契约与不变量

| 契约 | 说明 |
|---|---|
| 判空合成须显式标记 | `isEmptyComposition()` 是唯一合法判据。**绝不能用「Loaded 且无纹理」推断**——曾误伤 `test_tile_child_materializer` 2 条 + `test_sse_pipeline` 7 条 |
| 祖先交接双出口 | `overlayTileCannotRenderItself()` 必须同时覆盖 `Failed` **和** `isEmptyComposition()`——否则"祖先-only 空图"被 Step3 误提升为 ready、清空 `_pLoadingTile`,Step4 永远够不着触发,瓦永久空白 |
| 祖先查找不认空合成瓦 | `findLoadedTileOverlay` 显式跳过空合成候选,否则 `break` 父链遍历、**毒化整条子孙链** |
| 投影单位契约 | WebMercator 输出=弧度输入 × WGS84 长半轴(`6378137.0`);GCJ 内部用不同的 GCJ 参考椭球长半轴(`6378245.0`)——两者不可混用 |
| GCJ 单向变换 | 世界几何走 `fromWgs84`;源瓦矩形(本身已是 GCJ-02)不叠二次变换 |
| 跨中国框矩形不可用单一偏移代表 | `crossesChinaBounds()`——~500m 阶跃,调用方须按此判据分流 |
| mapping 失效靠 epoch | `directCompositeTileEpoch_`/`mappingRevision_`,`isCurrentProviderTile` 据此清陈旧 handle |
| PageStore 单域合成 | 同一 compose group 共享 canonical scheme/projection；异构组整组回退 Direct，domain generation 隔离迟到结果 |
| backend 替换不改配置所有权 | `RasterOverlayRuntime` 永远持有唯一有序 overlay 栈；backend 只能选择/消费 provider view，不得复制或重排 SDK 配置 |
| Direct slot 不压紧 | `RasterOverlayFrameContext::directOverlays()` 与配置栈等长；过滤层必须是 `nullptr`，不可改变后续 mapping/texcoord/binding 下标 |
| backend 切换有代际栅栏 | Direct view 变化先清旧 mapping/direct tile，再推进 generation；PageStore generation 独立，不让一条后端的启停误杀另一条后端的 source lease |
| source 请求只发一次 | Direct/PageStore 对同 provider/source key 共享 source depot；cache hit / join 不重复占用网络 grant |
| decoded 物理字节只算一次 | source cache、pending upload、PageStore external lease 可以重叠；`getSourceDepotResidentBytes()` 按 image identity 去重，cache eviction 后外部持有仍记账 |
| Resolution 与 sample 正交 | `RasterResolution` 不持有 Direct texture/UV；`DirectRasterSampleDescriptor` 只表达 Direct 采样，PageStore 保留 array+indir+placement 自己的 descriptor 形态 |
| CPU submission 期间禁止换租 | `TerrainPageStore::SubmissionLease` 覆盖命令构建→submit→引用释放；其间 determination/tick/reset/invalidate/reconfigure 拒绝 mutation |
| GPU 层只按 completion 回收 | `submittedSerial/completedSerial` 管理 pending use、retired layer 与 COW/staging 发布；固定帧延迟或对象保活都不能替代 completion frontier |
| Direct 仍是权威回退 | mixed-scheme、非真实地形、PageStore cold miss/failure/admission rejection 和祖先回退仍由 Direct mapping/composite 覆盖；在这些契约被新 backend 等价实现前不得删除 |

---

## 诚实得失

### ✅ 强项
- 祖先纹理复用零额外成本(刻意设计,已判死"烘祖先像素填洞"路径)。
- 单协议基类收敛(6 种协议共用 HTTP 缓存/decode/节流)。
- GCJ 矩形包络用区间算术而非中心点近似(更保守也更贵,8×8=64 次变换)。
- GCJ 被限定为"显式 opt-in 的采样层小气候",不污染其余子系统坐标假设(地形 ECEF、相机、模型、拾取、标准 overlay 一律保持 WGS84)。

### ⚠️ 短板 / 已知债
- `RasterOverlayTileProvider.cpp` **4785 行**(P4):四路失效逻辑已收敛部分重复,物理拆文件未做。
- **HttpCache 无过期/无 ETag 重验**(P1):真机对照影像重复请求曾达 14.9%(地形 0)。
- Raster 网络请求已保留 `Urgent/Normal/Preload` 语义并接入 Scene grant；当前 provider 源请求
  在真正调用 `ImageryProvider::requestTile` 前完成 admission，缓存命中与共享在途搭车不消费
  新请求额度。仍未统一的是底层 HTTP 实现自己的连接池 QoS。
- 空洞瓦每帧全量 `update()`(P3):未量化 CPU 成本。
- GCJ 缩放过渡态残差(P5):z≤5 时 px 级竖向残差(z5 约 7px),**标准 mercator 底图同样命中,非 GCJ 特有**。
- fill 代理 UV 仍走地形 scheme 投影(P6):GCJ 下二阶误差米级,未量化可见性。
- 中国框常量未经高德实际域比对——若不符,I-V12"退化有界"整体平移。
- WMS 只支持 1.3.0(I-V10):配 1.1.1 会同时错参数名和轴序。
- Google Maps Tiles 归属 logo 未做(I-V9,ToS 硬要求)。

> **历史教训**(memory):GCJ 缩放瞬间错位的真根因是**过渡期画面走 Direct composite 回落路而它漏了模板 remap**(早前只修 PageStore 路)。**审"已修"须枚举全部上屏路径(主路+回落路)**,只审主路被真机打脸;已判死 CPU warp(598µs/页 Release)。

---

## 扩展点

- **接新影像源协议**:继承 `XYZImageryProvider`,覆写 `buildUrl` 即可;会话 token 等前置逻辑放 `requestTile`,不改基类。
- **接新坐标偏移系**:三处联动——①`core/geodesy/` 新增变换器(`fromWgs84`/`toWgs84`/`boundRectangleFromWgs84`);②`RasterOverlayProjection` 新增枚举态 + `projectWorldPositionForRasterOverlay` 分支;③确认源瓦坐标系是否已是目标偏移系,是则 `projectRasterSourceRectangle` **不能重复变换**。
- **矢量伪装成影像源**先例:`VectorDrapeImageryProvider`,失败语义回全透明图而非 nullptr。

---

## 对照系

对齐 cesium-native `RasterOverlay`/`RasterOverlayTileProvider`/`RasterMappedTo3DTile`:LoadState 数值、7 步 update 流程、`SharedAssetDepot` 生命周期语义都是刻意对齐。**自研新增只有 GCJ-02**:`Gcj02WebMercator` 投影态及其变换器在 cesium-native 无对应物,专为高德等国内偏移底图源加的一层,且被限定为显式 opt-in 的采样层小气候。
