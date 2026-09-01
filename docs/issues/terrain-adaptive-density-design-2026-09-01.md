# 地形自适应密度多档 — 设计

> 冻结设计。代码落点 `TerrainDisplacementTemplatePool.{h,cpp}`、
> `TerrainDisplacementTemplate.cpp`、`TileRenderPlanFrameRefresher.cpp`、
> `DecodedHeightmapSampler.{h,cpp}`。
> 状态:2026-09-01 约定「本轮只出设计,不混入实现」。

## 问题

现状地形几何密度 = **二元** coarse 65² / dense 257²,SSE 驱动 + 迟滞
(`terrainGridSizeForSse`,`TerrainDisplacementTemplatePool.h:76-82`)。
- 默认相机大部分可见瓦片仍困在 coarse 65²=133m/三角形(数据是 16.6m),
  8 倍高程细节被网格吃掉 → 观感「块/刻面」(terrain.md T-V1,❌)。
- dense 只在近景**被 cap 瓦**触发,且 coarse↔dense 是**硬切换无 morph**
  (LOD 的 geomorph 不覆盖 density 轴)。

成熟引擎(Cesium quantized-mesh)用**自适应 TIN**,顶点密度跟地形误差走。
本项目不引入 TIN(另一套渲染体系),正解 = **多档规则网格,分辨率连续跟随
屏幕/地形误差,档间 morph**。

## 设计

### 1. 档位表取代二元
`TerrainDisplacementTemplatePool.h` 从固定 `kTerrainDisplacementGridSize`/
`kTerrainDenseGridSize` 两档,改为**档位表**(如 65²/129²/257²/513²,可配):
```cpp
struct GridTier {
    int gridSize;
    int heightArrayLayers;   // 该档高度 array 层预算
    int templateSlots;       // 该档模板槽位
    double acquireSsePx;     // 升档阈值
    double releaseSsePx;     // 降档阈值(迟滞 < acquire)
};
constexpr std::array<GridTier, kGridTierCount> kGridTiers = {...};
int terrainGridSizeForSse(double sse, int currentGridSize);  // 档间迟滞
```
- 档数定死(kGridTierCount),每档一套共享模板 VBO + 高度 texture2DArray
  (array 层边长是硬约束,同档各层必须等尺寸)。
- **决策单一事实源不变**:refresher 每帧盖章 `displacementGridSize`,
  resolver/LUT/探针/draw/CPU 采样全读它(现有 `decidedOrPredictGridSize`)。

### 2. 档间 morph(消灭硬跳)
现状 LOD geomorph(`hCoarse↔hFine`,`Renderer.cpp:942`)只桥 LOD。扩为**也桥
档位**:瓦片 morph 绑到 SSE 跨入本档频带,从上一档粗面 morph 到本档细节。
- `terrainMorphFactor` 推导从「只在 LOD 频带 (maxSSE/2, maxSSE]」扩为
  「LOD 频带 × 档位频带」的联合位置。
- shader 的双分辨率采样已有 hCoarse(2×自降采样)/hFine,可复用为档间过渡
  (但需确认 dense 模板顶点数变化不被 morph 单独承担——拓扑不能 morph,
  morph 的是**高度**,顶点都在椭球面位移,故档间 morph = 高度从粗面到细面,
  三角形密度瞬间变但表面连续)。

### 3. CPU/GPU 一致性契约
`DecodedHeightmapSampler::sampleHeightRenderGrid` 读 `terrainHeightGridSize`
(单决策点),多档时逐格一致防分叉——**新档位自动进 `RenderedTerrainSurfaceSampler`
revision**(已含 grid/morph/fade),矢量贴地零改动跟上。

## 代价账(铁律:不许填「应该很小」)
多一档 = 多一组高度 array + 模板池:
- 现状两档模板 VBO ≈ 49MB(coarse 16.9 + dense 32.3)、高度 array ≈ 17MB
  (coarse 4.3 + dense 12.7)。
- 加 129² ≈ 模板 4×coarse 量级 + 高度 array 一层 budget;档数越多,显存/上传
  线性涨。**落地前逐档量化**,与 T-V11(模板 VBO 有界)容量预算一起算。
- 档间 morph 需确认 dense 模板换档的顶点数暴涨不在 shader 里产生新 pop。

## 边界
- **不引入 quantized-mesh TIN**(另一套渲染体系,数天-数周)。若多档规则网格
  显存账不划算,再评估 TIN,那时另立专项。
- **不降任何可见细节**:自适应是「按误差加密」,不是缩短可见距离/降 LOD。

## 落地顺序
1. 先把几何侧 ②(事件驱动重钳)与 ①(height-only)合流——地形加密会让
   渲染面 revision 更高频,矢量贴地必须已就绪。
2. 档位表 + 档间 morph 的第一档扩展(2→3-4 档),先证真机观感/显存账。
3. 再决定是否上 TIN。

## 参考
- Cesium quantized-mesh 自适应 TIN(顶点密度跟地形误差);
- 本项目现有 geomorph(距离连续,hCoarse↔hFine)作为档间 morph 基础。
