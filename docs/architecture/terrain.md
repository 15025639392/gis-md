# 架构沉淀:地形内容子系统 (terrain)

> 这是**架构/方案**文档——回答"这个子系统怎么搭的、关键决策为什么这么定、接新功能从哪切入"。
> 质量标尺看 `docs/northstar/terrain.md`(T-V*/T-P* 判据);代码行号看 `AI_INDEX.md`;历史事故看 `docs/issues/*`。
> 行号锚点会随重构漂移,以符号名为准。
> 从相机、选择、加载、RenderPlan 一路到提交的跨模块执行顺序见 [terrain-runtime-pipeline.md](terrain-runtime-pipeline.md)。

**规模**:`content/` 17k 行 + `tiling/` 里的高程服务与 GPU 位移基础设施。引擎重心三大件之一。

---

## 职责边界

**管**:
- **内容 provider 层**(`content/HeightmapTerrainContentProvider`、`EllipsoidTerrainContentProvider`、`CompositeTerrainProvider`):把某 `TileKey` 的地形请求变成可渲染的 `TileContentLoadResult`(glTF 模型 + 元数据),对上遵守 `TilesetContentProvider` 统一接口(对齐 cesium-native `TilesetContentLoader`)。
- **网格构建层**(`content/EllipsoidTerrainMeshBuilder`、`content/GltfTerrainUpsampler`):高度栅格/父模型 → 规则网格 `GltfPrimitive`。
- **GPU 位移基础设施**(`content/TerrainDisplacementTemplate`、`tiling/TerrainDisplacementTemplatePool`、`tiling/TerrainEdgeHeightLut`):共享零高程模板 + per-tile 高度纹理位移 + 跨瓦缝合。
- **原始数据获取/解码**(`providers/HeightmapTerrainProvider`):HTTP 拉 Terrarium/Mapbox-RGB PNG → `DecodedHeightmap`。
- **CPU 侧统一高程查询**(`tiling/TerrainHeightService`,261 行):渲染线程用的地面高度门面,供矢量贴地/相机碰撞/包围体收紧消费。

**不管**(交给消费方):GPU 渲染命令组装(`tiling/GltfDrawCommandBuilder`)、地形纹理页存储/影像叠加(`renderer/TerrainPageStore`)。本文只在数据流末端触达它们。

**已死掉、勿再引用的三套旧实现**:Quantized Mesh 全路径(`QuantizedMesh*`,已删)、`terrain/TerrainTile`(OpenGlobus 遗留的第三套高度采样,2026-08-07 删)、`GlobeMesh`/`Globe`/`TileQuadTree`(旧自研全球网格栈)。

---

## 核心设计决策 + 理由

### 1. 走 glTF 渲染路径,而非专用 surface mesh
引擎从"自研 `BasemapLayerStack`/`TileQuadTree` + 专用地形网格"整体迁移到"cesium-native 对齐的 `Tileset` + glTF 3D-Tiles 内容模型"。地形现在被合成为规则网格 `GltfPrimitive`(`EllipsoidTerrainMeshBuilder::makeModel`),走与其它 3D Tiles 内容**完全相同**的解码→上传→绘制管线。

- **收益**:复用整套 glTF 资源准备(纹理槽、PBR uniform、raster-overlay UV),不为地形单独维护 VBO/绘制/材质管线。
- **代价**:地形顶点受 glTF 顶点格式约束。为此切出 32B `TerrainGpuVertex`(POSITION+NORMAL+TEXCOORD_0,`static_assert(sizeof==32)`)做轻量特化,而非重设计地形专属顶点。

### 2. 共享网格 + GPU 纹理位移的同构设计 ★
地形几何链最重要的性能决策(`content/TerrainDisplacementTemplate`、`tiling/TerrainDisplacementTemplatePool`):

- **几何性质**:ENU 局部帧随经度旋转,因此同一 `{LOD, mercator-row}`(同纬度带、同经度宽度)的所有瓦片,其零高程栅格在 ENU 帧下的 `localPos`/`localNormal` **逐列不变**(经度平移=绕 Z 旋转,被瓦片自己的 ENU 帧抵消)。
- 于是同一行所有瓦片共享一份模板 VBO/IBO(缓存键 `{schemeId, z, row, gridSize, latSpan, lonSpan}`),per-tile 经纬度落位改由刚体变换 `enuToEcef(tileCenter)` 折进模型矩阵。
- 真实高程用 **per-tile 高度纹理**(`gridN+1` 方 RGBA8,RG 打包 16bit 归一化高度 + BA 打包切空间法线 xy)在 shader 里位移:`pos = 面点 + 法线·h`。
- **收益**:地形 VBO 字节从"随可见瓦片数线性增长"压到"每 `{LOD,row}` 一份"(北极星有界性闸门),差异性搬进小纹理(共享 `texture2DArray`,256 层 LRU)。
- **等价性证明**:模板+高度纹理的采样约定与 CPU 烘焙路径 `EllipsoidTerrainMeshBuilder` 逐字对齐(纬度 north v=0→south v=1、经度反经线安全、索引缠绕 `a,c,b,b,c,d`),代数上 `world = frame·(localPos + localNormal·height)` 等价于 `cartographicToCartesian(lng,lat,height)`(P0 test `test_terrain_gpu_displacement_precision` 佐证)。

> **踩过的坑(T-V9,已修)**:模板由**第一个来要的瓦片** bounds 定型并永久缓存、不自愈。两套切片方案(`rootTilesX` 差一倍)瓦片跨度不同却落同一缓存键 → 后来者拿到半宽模板、几何只铺一半、四周露背景(真机:`spanMis>0` 帧 dark 均值高 245 倍)。根修=地理跨度并进缓存键。**但谁撞了谁(T-P5)未定位**,只加兜底,没修元凶。

### 3. 密度 65×65 钉死 + 后来的两档自适应
- `kTerrainDisplacementGridSize = 64`(2^6+1,GE 嵌套栅格约定)。
- **代价已量化**:z12 瓦片地面宽 8510m/64 格 = **133m/顶点**,而源数据 514px = 16.6m/px,8 倍高程细节从未上 GPU;对照 Cesium World Terrain 自适应 TIN 有效精度 10-30m,**粗一个数量级**。
- **为何不全局抬密度**:真机峰值可见 103 片,全局 64→128 即 ≈3.4M 三角/帧,手机不可行(现状 844k)。
- **实际取舍=两档**:`kTerrainDenseGridSize=256`,SSE 驱动升降档(阈值 64px 升/48px 降,25% 迟滞防抖)。只两档不连续,因"每多一档多一个高度纹理 array"。
- **单一决策点**:`decidedOrPredictGridSize` 每帧决策一次,draw/接缝 resolver/边高度 LUT/错位探针全经它读——此前四处各自从 SSE 重推、迟滞不一致互相打架。
- **仍未解决**:T-V1 抬 256 全局"未实测";且这仍是"密度自适应"而非"几何误差自适应 TIN"。

### 4. 无缝方案 B:边高度 LUT(而非逐瓦片量化)
- **被否(已判死)**:逐瓦片高度量化——破坏无缝所需的逐位相等。
- **选中 B**:`TerrainEdgeHeightLut` 在 resolve 阶段用 CPU 真实数据把"粗邻居在吸附节点处实际渲染出的高度"逐节点算好,写进每瓦片高度纹理下方额外 4 行(`kEdgeLutRows=4`),shader 拿它取代自纹理吸附——两侧在共享边上求值**同一函数**,是恒等而非逼近。
- **为何不走方案 A(GPU 采邻居纹理)**:①每条边面向不同邻居,实例流翻倍;②coarse(65²)/dense(257²)是两个独立 texture array,吸附恰在档边界最常触发,单 sampler 采不到跨档邻居。
- LUT 存**差值**(固定量程 ±2048m,步长 0.0625m),不存绝对高度——绝对高度要跟每瓦片自己的 `(minH,range)` 反量化,量程不同会静默截断。
- **裙墙同属此体系**:裙顶点与边顶点逐字一致但标记"跳过位移"(哨兵 heightDelta=-1),自动形成"位移后边缘→椭球面"的墙,零过冲、无需 `skirtHeight` 常量。根治了旧版固定 `skirtHeight` 在粗 LOD 膨胀成 24-385km 巨墙。

### 5. no-data 三层兜底
1. **数据源哨兵**:Terrain-RGB 的 `RGB(0,0,0)` 解码恰为 -10000m,解码器**无条件**追加该哨兵(`kTerrainRgbNoDataFloorMeters`);显式+隐式合并才是生效哨兵表。
2. **采样剔除**:`makeHeightSampler` 对 `isNoData` 返回 `nullopt`,不把 -10000 当真实高度喂三角形。
3. **边界法线 no-data 剔除**(T-V10 根因①):生产 NASA 514 源普遍缺西/北重叠环(19 片样本 14 片整列/行 `code=0`),把 no-data 当 0m 送边界差分会把法线打到近水平(82.28°→修后 <3°)。修法=no-data 邻居退化单边差分;**GPU 烘焙侧曾漏移植此逻辑,静默分叉很久**(印证 T-P6 债)。

### 6. CPU 统一高程服务为何独立
- **不是数据源,是索引层**:建在既有 retained `DecodedHeightmap`(与 GPU 位移同一份数据源)之上的带索引查询门面——零拷贝、不建金字塔、不做 IO;miss 返回 `nullopt`,调用方显式决定兜底。
- **为何独立**:矢量贴地/相机碰撞/包围体收紧都要"某经纬度地面高度",各自扫 registry 是 O(n);本服务按 zoom 分层 cell 哈希索引,`heightmapGeneration` 强代次驱动惰性重建,稳态零重建。
- **生命周期安全靠结构性论证**:索引存裸指针,安全建立在"registry 从不 erase 单瓦" + "服务是 Tileset 成员、tileset 亡则服务亡";正确性不依赖新鲜度——命中后二次验真(`isTerrainRenderContent`+heightmap valid+bounds.contains),过期条目零危害。
- **`heightRangeForArea` 走矩形而非 TileKey 上溯**:计划瓦片 key 未必与带高度图的瓦片同网格(真机祖先链算 203/106,索引里实际 202/107),矩形是两套网格唯一共通坐标。

---

## 数据流(关键路径)

```
1. HeightmapTerrainProvider::requestTile
   → HTTP GET (curl) 拉 Terrarium/Mapbox-RGB PNG → decodeTile → DecodedHeightmap

2. HeightmapTerrainContentProvider::buildContent(key, heightmap, options)
   → makeHeightSampler (web-mercator 投影 + 双线性 + isNoData 剔除)
   → EllipsoidTerrainMeshBuilder::makeModel → 单个 GltfPrimitive(+ skirt)
   → TileContentLoadResult::renderTerrain(...)  // terrainRenderContent=true

3. CompositeTerrainProvider 路由(primary 未覆盖 → EllipsoidTerrainContentProvider 椭球兜底)

4. GltfRenderResourcePreparer::prepareCpuWork(worker)
   → useTerrainFormat = primitive.hasTerrainWaterMaskMetadata
   → CPU 侧固定顶点 → uploadToGpu 建 VBO/IBO/纹理

5. GltfDrawCommandBuilder::build(每帧)
   → GPU 位移开启: pool.acquire(共享模板) + acquireHeightTexture(per-tile 层) + edgeLut 写实例流
   → renderer.makeTerrainPrimitiveCommand(stride 32) 或 makeGltfPrimitiveCommand(stride 120)

6. Renderer(GLES: kTerrain*GLSL / Metal: kTerrain*MSL)
   → VS: pos = 面点 + 法线·h(texelFetch 反量化), 纬度衰减 terrainReliefFade(z)
   → FS: 边界法线/relief 光照 → 上屏

上采样链路(覆盖边缘"纯洞四元组"):
  TileChildMaterializer → GltfTerrainUpsampler::upsampleForRasterOverlay
    (裁父模型到子象限 + 重归一化 windowed texcoord + 传播水位平移/缩放)
```

---

## 关键契约与不变量

| 契约 | 出处/说明 |
|---|---|
| 顶点格式二选一门闸 | `useTerrainFormat = primitive.hasTerrainWaterMaskMetadata`——唯一决定 32B `TerrainGpuVertex` 还是 120B `GltfGpuVertex` |
| `terrainGpuVertexBytes` 已无生产者 | QM 退役后无人预建;地形**永远**走 CPU 顶点路径构建 |
| 采样约定单一事实源 | `TerrainDisplacementTemplate` 与 `EllipsoidTerrainMeshBuilder` 逐字对齐纬度方向/反经线/索引缠绕 |
| 档位单一决策点 | `decidedOrPredictGridSize` 是唯一合法读法,禁止各自从 SSE 重推 |
| 共享模板缓存键必须含地理跨度 | `{schemeId,z,row,gridSize,latSpan,lonSpan}`,缺跨度→跨方案模板错配且永不自愈(T-V9) |
| draw 侧与 CPU 侧渲染网格采样一致 | `DecodedHeightmapSampler::sampleHeightRenderGrid`(矢量贴地)必须与 draw 侧同函数,否则贴地矢量浮起/陷入 |
| nodata 哨兵单一来源 | `kTerrainRgbNoDataFloorMeters=-10000`,注册点与契约检查 `contracts::Id::DemNodataSentinel` 共用常量 |
| `no-fine-data-ellipsoid-fallback` | primary provider 未覆盖处**必须**报 `NotAvailable`,`CompositeTerrainProvider` 才能路由椭球兜底 |
| 有意不做 | 不隐藏覆盖区/椭球区之间的真实高度悬崖(高原+4km 接海平面 0),只消巨型裙墙与粗台阶 |

---

## 诚实得失

### ✅ 强项(附证据)
- **加载期不再硬冻结**(T-V4):上传改 budget 涓流,A/B 积压 48→0、暂态 0.53s→0(`294bff2ed`)。
- **稳态无缝已收官**(T-V5):边高度 LUT + 档位单一决策点,稳态漏天精确 0。
- **瓦界发白细线已修**(T-V10,2026-08-16):根因(源重叠环 no-data 当 0m 参与边界差分)定位精确,CPU/GPU 双修,有对照(夹角 82.28°→<3°)。
- **共享模板结构性设计经受真实事故验证**(T-V9,修法是结构性根治非补丁)。
- **裙墙无过冲**:哨兵顶点跳过位移,数学保证绝不伸到椭球面以下。
- **TerrainHeightService 有对拍守卫**(`test_terrain_height_service.cpp` 与旧实现等价验证)。

### ⚠️ 短板 / 已知债(来源:`terrain.md` T-P*、AI_INDEX)
- **几何密度钉死粗一个数量级**(T-V1,推断):65×65 在 z12 只有 133m/顶点,抬 256 全局代价"顶点 16×、显存 16×"未实测。
- **光照压平 relief**(T-V3):**代码已修复**(2026-08-12,`1a939be70`,线性 Lambert 单一治理点 `TerrainSurfaceLightGLSL.h`,弃用 smoothstep);剩余 = 真机 A/B 拍板观感 + MSL sunTint 参数化(B4)。不再闸住 T-V1/T-V2。
- **Metal 后端地形能力不对等**(T-V8/T-P1/T-P2):Metal 从未绑定地形高度纹理,GPU 位移在 Metal 上休眠;GPU 烘焙仅 GLES。均有安全回退,是"未做"非"做错"。
- **GPU shader 无执行级守卫**(T-P6):host 测试无真 GL,GLSL 只能真机肉眼验;T-V10 根因①正是 GPU 侧漏移植静默分叉的直接证据。
- **fill 代理同步无帧预算封顶**(T-P3/T-V6,未量化):每可见瓦片循环 `ensureFillProxy` 无 break/budget,只在首见/换页帧密集暴露。
- **T-P5 模板键碰撞元凶未定位**;**T-P4 HDR 常数 provisional**(`shadowFloor=0.15`/`ambientScale=0.6` 未定)。
- **T-V7 椭球兜底观感未真机验**(单测 187/187 通,画面悬崖只单测层验证)。
- **无阴影、无 AO**(全库零命中),判定为密度不到位时加阴影意义不大,列第二梯队。

---

## 扩展点

- **接新地形源**:实现新 `TilesetContentProvider` 子类(参照 `HeightmapTerrainContentProvider`),产出符合契约的 `TileContentLoadResult::renderTerrain(...)`,下游 draw/GPU 位移/高度查询自动可用(`renderTerrain` 工厂是唯一硬契约点)。部分覆盖源套一层 `CompositeTerrainProvider` 获椭球兜底;注意 `availabilityState` 必须空间性,否则 `GltfTerrainUpsampler` 触发路不可达。
- **新位移方式**:切入点 `TerrainDisplacementTemplatePool::acquire`/`acquireHeightTexture`。改位移公式改 shader `pos = 面点 + 法线·h` + `bakeTerrainHeightNormalTexels` + GPU 烘焙 shader,两侧保持单一事实源。
- **接密度第三档**:`heightArrays_` 理论支持任意 gridSize,扩展点在 `terrainGridSizeForSse` 加判据分支 + 新档共享 index buffer/height array,但要重估显存(每档一个 texture array)与迟滞。
- **新边缝合策略**:`TerrainEdgeHeightLut`/`TerrainEdgeLutTable` 独立模块,换算法改此处 + shader 侧 `eeSampleTerrainHeight` 反量化与吸附节点定位(两侧逐一对应)。

---

## 对照系

- **当前策略 = "共享零高程模板 + GPU 高度纹理位移 + 两档屏幕误差自适应"**。既非 cesium-native Quantized Mesh(服务端预压缩可变密度 TIN),也非 OpenGlobus。QM 已从本引擎**完全移除**——一次主动的架构简化:放弃服务端预烘焙可变分辨率网格,换客户端栅格 DEM(Terrarium/Mapbox-RGB)+ 固定密度网格 + GPU 位移。
- **精度代价**:Cesium World Terrain TIN 10-30m,本引擎 65×65 在 z12 是 133m/顶点,粗一个数量级。换来的是不依赖专有数据格式/服务端预处理,可直接消费任意 Terrarium/Mapbox-RGB 栅格源。
- **上采样对照**:`GltfTerrainUpsampler::upsampleForRasterOverlay` 对齐 cesium-native `upsampleGltfForRasterOverlays`;语义一致,但当前唯一触发场景是覆盖边缘补洞而非常规几何 LOD 细化(`decoupleImageryFromGeometry` 生产默认值**待与代码当前值对照**,标未验证)。
- **无缝方案对照**:QM 天然靠共享边缘顶点位置保证跨瓦无缝(数据格式层精确重合);本引擎放弃 QM 后无法在数据层保证逐位相等,故需边高度 LUT 在渲染时补偿——**用运行时计算换数据格式简单性**的取舍,代价是多一层机制与相应债(T-P5/T-P6)。
