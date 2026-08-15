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
- `RasterMappedToTilesetTile`(`tiling/`)——对齐 cesium `RasterMappedTo3DTile`:把「一张地形几何瓦」和「一张影像瓦」用三态挂接机(Unattached / TemporarilyAttached / Attached)绑一起。

**不含**矢量渲染——但 `VectorDrapeImageryProvider` 是例外:它把矢量面数据**伪装成影像 provider**接入这同一条通路(见扩展点)。

---

## 核心设计决策 + 理由

### 1. raster overlay → tileset tile 的映射
入口 `RasterOverlayTileProvider::mapRasterTilesToGeometryTile`(对齐 cesium `QuadtreeRasterOverlayTileProvider`):
1. 几何矩形按 `projection_` 反投影回地理,与 `sourceCoverageRectangle_` 求交——不在覆盖内直接返回 `{nullptr,...}`(**空洞的第一道来源**)。
2. `buildQuadtreeSourcePlan` 按 SSE/纹理尺寸算源 zoom + 源瓦矩形集。
3. **单源精确命中**直接返回原生瓦(`directTile=true`),不额外合成——最省的一条路。
4. 否则落"合成瓦"路径:以 `mappedRasterTileCacheKey`(含 `mappedRasterTileEpoch_`)查/建一张 `RasterOverlayTile`,后续异步拉多张源瓦调 `composeMappedSourceImageSet` 拼成一张纹理。

`RasterMappedToTilesetTile::update()` 是 7 步状态机:Attached 快路 → 失败/空合成回退父链 → 常规映射 → Loading→Ready 提升 → 祖先替身接管 → 失败无回退兜底 → attach → 报 `MoreDetailAvailable`。

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

---

## 数据流(关键路径)

```
ImageryProvider::requestTile (HTTP/bridge)
  → XYZImageryProvider::tryServeFromHttpCache (命中则下沉 worker 解码)
  → decodeTile (stb_image → DecodedImage)
  → RasterOverlayTileProvider 源瓦异步汇入 ProviderAsyncState (SharedAssetDepot 语义)
  → composeQuadtreeSourceImagesWithDetails / composeMappedSourceImageSet
     (多张源瓦拼成目标矩形图像;祖先-only 场景刻意返回 0×0 空图——设计如此,非 bug)
  → PendingUpload 入队
  → processPendingUploads (主线程):
       emptyImage → RasterOverlayTile::markLoadedWithoutTexture() (置 emptyComposition_)
       非空 → RasterTextureUploader::uploadRasterTexture → setTexture (置 Loaded)
  → RasterMappedToTilesetTile::update() 7 步机
       Step2: overlayTileCannotRenderItself(Failed || isEmptyComposition) → 走父链
       Step3: Loading→Ready 提升, computeTranslationAndScale 定 UV
       Step4: 祖先替身接管(ancestor-only 空洞的兜底)
       Step6: attachReadyTileInMainThread
  → SurfaceRasterBinding::chooseSurfaceRasterBinding (决定这帧画哪张纹理+哪套 UV)
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
| mapping 失效靠 epoch | `mappedRasterTileEpoch_`/`mappingRevision_`,`isCurrentProviderTile` 据此清陈旧 handle |

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
- `FrameResourceBudget::canIssue` 忽略优先级(P2):帧级延迟,非持续饥饿。
- 空洞瓦每帧全量 `update()`(P3):未量化 CPU 成本。
- GCJ 缩放过渡态残差(P5):z≤5 时 px 级竖向残差(z5 约 7px),**标准 mercator 底图同样命中,非 GCJ 特有**。
- fill 代理 UV 仍走地形 scheme 投影(P6):GCJ 下二阶误差米级,未量化可见性。
- 中国框常量未经高德实际域比对——若不符,I-V12"退化有界"整体平移。
- WMS 只支持 1.3.0(I-V10):配 1.1.1 会同时错参数名和轴序。
- Google Maps Tiles 归属 logo 未做(I-V9,ToS 硬要求)。

> **历史教训**(memory):GCJ 缩放瞬间错位的真根因是**过渡期画面走 mappedRaster 回落路而它漏了模板 remap**(早前只修 pageStore 路)。**审"已修"须枚举全部上屏路径(主路+回落路)**,只审主路被真机打脸;已判死 CPU warp(598µs/页 Release)。

---

## 扩展点

- **接新影像源协议**:继承 `XYZImageryProvider`,覆写 `buildUrl` 即可;会话 token 等前置逻辑放 `requestTile`,不改基类。
- **接新坐标偏移系**:三处联动——①`core/geodesy/` 新增变换器(`fromWgs84`/`toWgs84`/`boundRectangleFromWgs84`);②`RasterOverlayProjection` 新增枚举态 + `projectWorldPositionForRasterOverlay` 分支;③确认源瓦坐标系是否已是目标偏移系,是则 `projectRasterSourceRectangle` **不能重复变换**。
- **矢量伪装成影像源**先例:`VectorDrapeImageryProvider`,失败语义回全透明图而非 nullptr。

---

## 对照系

对齐 cesium-native `RasterOverlay`/`RasterOverlayTileProvider`/`RasterMappedTo3DTile`:LoadState 数值、7 步 update 流程、`SharedAssetDepot` 生命周期语义都是刻意对齐。**自研新增只有 GCJ-02**:`Gcj02WebMercator` 投影态及其变换器在 cesium-native 无对应物,专为高德等国内偏移底图源加的一层,且被限定为显式 opt-in 的采样层小气候。
