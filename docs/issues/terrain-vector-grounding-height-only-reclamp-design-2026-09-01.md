# 矢量贴地 height-only 重钳重构 — 设计

> 冻结设计(写完即归档)。代码落点在 `scaffold/src/earth_engine/layers/FeatureRenderLayer.{h,cpp}`。
> **2026-09-01 已实施**:`buildTileSymbolGpu` 拆成 `resolveTileSymbols`(样式/图集/选中,
> 缓存 `activeResolvedSymbols_`)+ `materializeTileSymbols`(采当刻地形高 + 点/标签物化);
> `reclampTileBucketSymbols` 改 height-only —— 复用缓存 resolved 重物化,不重 resolve,
> 保留 crossTile id、失效标签重烘但不重启 placement。host 新增
> `HeightOnlyReclampPreservesSelectionAndUpdatesHeights` 锁「高度更新 + 选中集签名不变」。

## 背景与目标

`reclampTileBucketSymbols`(`FeatureRenderLayer.cpp:4229`)当前经
`rebuildTileBucketSymbolsForZoom(gpu, zoom, force=true)` 触发**整桶重建**:
`buildTileSymbolGpu` 把「选中集判定 + 官方样式解析 + 图集查询 + 高度采样 +
点 quad 构建 + 标签源构建」**全部交织**在 200+ 行单块里(`:3857-4081`)。

地形 revision 变化(冷启动细化 / morph / 换档)时,每个符号桶被整桶重跑——
其中选中集、官方样式、图集在纯高度变化下**都不变**,只有锚点高度变了。
目标:拆出**单一 height-only 重钳路径**,只重采高度 + 重物化点/标签 quad,
跳过 style / atlas / selection / placement,不保留双套。

## 关键约束(源码核实的不可拆分点)

1. **「选中哪些符号」依赖外观解析。** `buildTileSymbolGpu` 每个符号的
   `continue`(进入/跳过)由官方样式解析结果决定(`providerArtworkReady`、
   `providerZoomOverride`,`:3994-4007`)。因此 **active 集不是 style 无关的**。
   → 不能只缓存外观,必须缓存「已解析的 active 符号 + 外观」。
2. **标签 quad 位置依赖锚点高度。** `TileLabelSource` 存 `relF/anchor/`
   `tangentRelF/tangent/labelPath`(`:4066-4068`),高度变了 quad 必须重烘。
   → height-only **不能**跳过标签重烘,只能跳过 placement 碰撞重跑。
3. **placement 不该因高度重钳重启。** V29 的 fade 账本按 crossTile id 键,
   placement 节流 300ms 只该管碰撞重算;重钳不应重启 fade。

## 重构方案

### 新中间结构:已解析 active 符号
每桶缓存:
```cpp
struct ResolvedTileSymbol {
    const TileSymbolCpu* src;         // 源(锚点 lon/lat、labelPath)
    TileSymbolAppearance appearance;  // icon/color/size/labelLayout/paintOrder/
                                      // minZoom/maxZoom/rank/officialCanCovered
                                      // (一次解析,height-only 复用)
};
// BucketGpu 内:
std::vector<ResolvedTileSymbol> activeSymbols_;  // 当前 active 已解析集
```

### 拆两段
- `resolveTileSymbols(gpu, viewZoom, officialScale)`
  → 重算 active 集 + 外观,**仅在 selection/style/zoom 变化时跑**。
- `materializeTileSymbols(gpu, activeSymbols, sampler)`
  → 对每个已解析符号:采样高度 → 算锚点 ECEF → 建点 quad → 建标签源
  (labelPath 带新高度)。**height-only 只跑这一段。**

### height-only 单一路径
`reclampTileBucketSymbols` 改为:
```cpp
if (selection 未变 && 仅地形 revision 变化)
    → materializeTileSymbols(gpu, gpu.activeSymbols_, 当前 sampler)
    → 重建点 VBO + 标签 VBO
    → **不** invalidate placement / 不重启 fade
else
    → resolveTileSymbols + materializeTileSymbols(全量,原路径)
```
判定复用现有 `symbolSelectionSignature`(`:4169`):signature 匹配 = 选中集未变
→ 走 height-only;否则全量。**单一路径**,不保留 force=true 的双语义。

### 标签处理
- height-only 重物化后标签 quad 新位置进 VBO;
- placement 的 `opacities`(按 crossTile id)与 fade 账本**不清**(`:4187-4193`
  已有按 name 保留 featureId 的先例,height-only 沿用);
- 碰撞**不重跑**(几米高度差不改屏上重叠结论)。

## 测试
- host:height-only 重钳后点/标签顶点高度更新、选中集与 crossTile id 不变、
  placement 状态不重启(fade 不归零)、pendingReclamp 收敛。
- 真机:重庆默认视角冷启动 + 推近,POI 不消失、标签不重排。

## 代价
- 单桶多一份 `activeSymbols_`(≤128 个 ResolvedTileSymbol,字节级)。
- height-only 仍要重物化点/标签 quad(必要),省掉的是 style/atlas/selection/
  placement 重跑。
- 不降低任何可见细节,不引入第二套渲染路径。

## 已确认不做
- 不改遮挡 shader(`eeSymbolTerrainVisibility` 保持现状,靠 ② 事件驱动防陈旧,
  不引入幽灵残影)。
