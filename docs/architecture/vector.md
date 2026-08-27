# 架构沉淀:矢量子系统 (data + layers + style)

> **架构/方案**文档。质量标尺看 `docs/northstar/vector.md`(528 行,V*/P* 判据 + 已判死清单,信息最全);机制/扩展另见 `docs/mvt-vector-architecture.md`。行号随重构漂移,以符号名为准。

**规模**:`data/` 6.2k + `layers/` 5.1k + `style/`。

> ⚠️ **本仓矢量实为两套并存的子系统,勿混为一谈**:
> 1. **FeatureStore 编辑矢量层**(`FeatureRenderLayer` + `data/FeatureStore`,P1-P6 分期)——GeoJSON 导入、可编辑、贴地走"几何 clamp(方案A)+ stencil 分类(方案B)"。
> 2. **MVT 底图管线**(`MvtVectorSource`/`VectorTileTree`/`RoadFieldSource`,刀1-刀2+符号五刀+场专项)——只读底图,面/线/点三分工各换表示。
>
> 两者共享部分基础设施(`StyleExpression`/`StyleFilter`/`FeatureRenderLayer` 承载 MVT 瓦片桶路径),但数据流、贴地方案、样式表达都不同。

> ⚠️ **生命周期状态：MVT 道路线场即将废弃。** 具体范围是
> `RoadFieldSource` → `LineFieldRasterizer`/D2 → `TerrainPageStore` 场平面这条兼容链路。
> 现有实现暂时保留，用于兼容、回归和历史性能对照；新功能、新图层不得继续依赖或扩展该路径。
> 这不等同于废弃 MVT 面/点，也不等同于废弃 `FeatureRenderLayer` 的几何线渲染；替代实现和删除时间尚未在本文确定。

---

## 职责边界

| 子系统 | 输入 | 承载 | 不做 |
|---|---|---|---|
| **获取层** `MvtTileFetchCache` | MVT `.pbf` URL | fetch+解码+在途去重+两层 LRU(L1 解码瓦 48、L2 压缩字节 256) | 不做"要哪些瓦"的选择 |
| **FeatureStore 编辑层** `FeatureStore`(90 行)+ `FeatureRenderLayer`(2192 行) | GeoJSON `Feature` | 增删改查、镶嵌、贴地、拾取、编辑预览、避让 | 引擎只暴露 pick/snap/preview 三接口,不做编辑器逻辑 |
| **MVT 面(drape)** `VectorDrapeImageryProvider`+`VectorTileRasterizer` | MVT `fill` 层 | 冒充 `ImageryProvider`,栅格化进影像页存储 | 不产生独立 draw call |
| **MVT 线(SDF/D2 场，⚠️即将废弃)** `RoadFieldSource`+`LineFieldRasterizer` | MVT `roads` 层 | 兼容性保留：烘焙"线段纹素"场,寄生地形 FS 解算贴地线宽 | 不得新增依赖；场编码无图层/分色通道(V9 dash 未做直接原因) |
| **MVT 点/标注** `MvtVectorSource`+`VectorTileTree`→`FeatureRenderLayer` 瓦片桶 | MVT `poi` 层 | 瓦片即桶、worker 全链镶嵌、billboard+SDF 标注、避让 | 不进场(会随距离缩到不可读) |
| **样式** `StyleExpression`/`StyleFilter`、`VectorRasterStyle`、`FeatureRenderStyle` | 样式配置 | 表达式求值、层过滤、分级 | **三套样式模型互不统一** |
| **GPU 合成治理点** `eePageStoreCompose` | 影像/场页存储 | 唯一实现,注入六个 shader,完成影像+场一次合成、线宽像素解算 | 不允许各 shader 各写一份 |

---

## 核心设计决策 + 理由

### 1. FeatureStore 中心设计
`FeatureStore` 是编辑矢量层唯一要素数据源;`FeatureBucketGrid` 按经纬度格分桶,桶即 GPU 上传粒度。理由:要素级增删改与 GPU 资源生命周期解耦——改一个要素只需重建它所在的桶。`FeatureSpatialIndex`(R 树)与 `FeatureClusterIndex`(点聚合,只出索引不出渲染)都建在其上。**MVT 路径复用同一个 `FeatureRenderLayer` 但走"瓦片即桶"**——瓦片本身就是天然空间分桶,不需要 FeatureBucketGrid。

### 2. 瓦片即桶 + worker 三角化(E1)
MVT 把"一张瓦片"直接当渲染桶,镶嵌在 worker 完成——峰值 16107ms→216ms。**worker 边界严格**:可做样式过滤求值/MVT→Feature/镶嵌/栅格化/场烘焙;**不可碰图集/GL/地形采样器**。所以点符号 worker 只产 `TileSymbolCpu` 实例表(经纬度锚点+已求值样式),展开 quad/查图集/采地面高留给渲染线程的 `commitTileMesh`;文字同构,`bakeTileBucketLabels` 字体就绪时补烘(设计成幂等)。**锚点存经纬度不存 ECEF**:贴地采样是渲染线程状态,worker 给 ECEF 等于把高度焊死在椭球面,山地下整批被地形"埋掉"。

### 3. 贴地方案 A(几何 clamp)+ 方案 B(stencil)取代 draping/RTT
引擎当时 `createFramebuffer` 直接 `return nullptr` → RTT 全线不可用 → osgEarth draping / GPU-depth-RTT 钳制**结构性封死**。三候选(A CPU 顶点采高偏移 / B stencil 阴影体分类 / C GPU 高程 shader 钳制,受 LOD 限未选)定案"分而治之":
- **点/marker/标注锚点/编辑手柄** → 方案 A。数量少、采样器现成,拖拽实时贴地反馈只有 A 能给。
- **密集线/面(静态几何)** → 方案 B 为终态。"globe 上唯一地形 LOD 切换仍正确+像素精确+不依赖 RTT"的方案。
- 方案 A 踩过结构性坑:格内起伏差沉入=渲染面(粗节点插值)与贴地几何(全分辨率采样)之差恒定存在的穿插,根修=改用 `sampleHeightRenderGrid` 同源采样。方案 B 踩过更大坑:真机整个体积侧影被染色,三种 stencil func 探针画面完全相同 → 根因是离屏 FBO 深度附件无 stencil 位。**方法论**:改 func/ref 探针画面不变 = stencil 没跑,先查渲染目标有无 stencil 附件,别怀疑 op/winding。

> ⚠️ 上面"drape"指 **RTT 意义的 draping**,与 MVT 底图"面"用的 **drape imagery compositing**(刀1,栅格化后并入影像页合成)是完全不同的机制,不要混淆——后者恰是"表示随负载"换掉几何贴地的产物。

### 4. 符号统一路径
点/标注不管来自 FeatureStore 还是 MVT,最终都走 `FeatureRenderLayer` 的 billboard quad 链:worker 出实例表 → 渲染线程展开 quad+查图集+采地面高 → 发 `VectorPoint`/`VectorLabel` 命令。**渲染固定状态按类型分道**:`VectorFill`/`VectorLine` 强制 `depthTest=true`(贴地几何像素与 3D 位置一一对应);`VectorPoint`/`VectorLabel` 强制 `false`(billboard 四角共用锚点深度,逐像素深度测试会切出不存在的形状边界)。符号遮挡改由锚点判定(连续量非布尔避免临界闪;屏幕空间标定容差非固定米数)——与 maplibre/osgEarth/Cesium 三家同解。

`FeatureStore` 业务要素已接入 Scene/Engine 统一 picking：Scene 只编排候选和
排序，几何命中仍由 `FeatureRenderLayer::pick()` 负责；统一结果同时带有
layer/feature 身份、渲染位置和 Vertex/Edge/Fill 细节。MVT source 的生命周期
已统一托管，但瓦片桶当前仍是“渲染已接通、逐要素 picking 未接通”：
`FeatureTileMesh`/`TileSymbolCpu` 没有稳定的 source-layer/feature-id 回链。
在补齐 per-feature picking payload 之前，不应把 tile mesh 的跨瓦片 label ID
或瓦片键伪造为业务 `featureId`。

### 5. 样式表达式树(色烘顶点 / 宽走 uniform)
`StyleExpression` 是表达式求值树(⚠️ `String` 必须用独立命名的 `literalString`,与数字重载会歧义)。颜色类表达式在镶嵌期求值后**烘进顶点属性**;线宽/点大小类**不烘顶点,走 FS/VS uniform 逐帧求值**(按相机高度换算 zoom 后 evaluate)——理由是宽度需连续响应相机变化(屏幕像素恒定语义),烘进顶点会在 LOD 切换/相机移动时不连续或需重镶。MVT 场路径更激进:场编码里完全没有颜色/分类通道,线色是 FS 里**一个全局 uniform**——"表示选得便宜"的对偶代价,直接卡死 V9(dash)与多色路网。

### 6. MVT 底图三分工(面/线/点各换表示,不是各自优化；线场为⚠️即将废弃的兼容路径)★
判据(用户认可):**矢量=数据模型,渲染表示是引擎内部自由;表示随负载,判据=体验达标且最便宜**。三条选择理由:
- 面(水/landuse)视觉本质是**归属+颜色** → 栅格化进影像页合成(刀1)。距离场表达不了"归属";几何 stencil 实测 ~75ms GPU(发热真凶,已根除)。
- 线(路网)视觉本质是**到中心线的距离** → **历史兼容选型**：SDF/D2 距离场 + FS 解析解算(刀2)。几何 ribbon 掠视块状;独立 overlay pass 实测 25-30ms;RTT 位图线宽会在两次重烘之间漂。
- 点(POI/标注)视觉本质是**屏幕注记** → billboard + 锚点级遮挡。进场会缩到不可读;逐像素深度测试会把 quad 切一半。

三条路**互相不可替代,也不该强行统一**——northstar E 节记录多次"统一"尝试被实测打回。代价是**三条路代码几乎不共享**。

### 7. 场线宽像素一致的 D2 线段纹素方案(场专项定稿；⚠️即将废弃)
`LineFieldRasterizer.h` 注释即设计文档:**为什么不是距离场**——标量/向量距离场都靠双线性插值重建,插值本身是伪影之源(标量跨线心尖点必高估、真实路网实测漏画 63.1%;向量跨双线中轴必过零→幽灵)。D2 改为**每纹素存最近线段的局部参数**而非距离值:RGBA8 = 最近点偏移(ox,oy,±4 texel)+方向角 θ + fwd/back 端点剩余长度。FS 取 2×2 邻域 4 条线段各自解析算胶囊距离取 min——**全程无插值**。端点余量让胶囊在真实端点收口、拐角两胶囊圆帽相接成天然圆角。模拟对拍 texelPx=4:漏画 63.1%→0.28%,幽灵→0.000%,误差 0.025px。A==0 是空哨兵(failure-safe),FS 靠"像素可被线覆盖 ⇒ 所在纹素必有记录"做单 fetch 早退。
> ⚠️ 已判死弯路:分母用 `fwidth(fieldV)`(线心是场脊,导数→0,沿线心挖洞)、用 `dFdx(sampleUv)`(span 边界回绕爆导数,白虚线)、宽度烘进场纹素(与"屏幕像素恒定"冲突)。

---

## 数据流(关键路径)

### MVT 底图(面/线/点三分工)
```
MVT URL → MvtTileFetchCache(唯一获取层,L1解码48/L2压缩256,失败不入缓存)
   → shared_ptr<const MvtTile> 扇出三路
   ├─【面】VectorDrapeImageryProvider → VectorTileRasterizer::rasterizeMvtRect
   │       → RGBA8 位图(失败回全透明图,非nullptr)→ TerrainPageStore 影像平面(512层×256²×4B=128MB上限)
   ├─【线，⚠️即将废弃】RoadFieldSource → LineFieldRasterizer::rasterizeLineFieldRect
   │       → D2 RGBA8 线段纹素 → TerrainPageStore **场平面**(兼容路径；独立zoom封顶/独立LRU,64层16MB封顶)
   └─【点】MvtVectorSource::update → VectorTileTree(R*选择/LRU)
           → worker 镶嵌 → commitTileMesh(渲染线程,采地面高+查图集展开quad)→ 整瓦原子替换 → RenderCommand(VectorPoint/Label)
   面/线 → 地形 FS 内一次合成 eePageStoreCompose()(唯一治理点,寄生地形 pass,GPU~0)
   点   → 独立 draw(order 30/31,深度测试关,遮挡只看锚点)→ 上屏
```

### FeatureStore 编辑层
```
GeoJSON → GeoJsonParser → GeoJsonImporter::importInto → FeatureStore::addFeature
   → FeatureBucketGrid(经纬度格分桶)→ syncDirtyBuckets(脏桶重建)→ tessellateFeatureInto
     ├ 面/线贴地:prepareClampedFeature(方案A,边细分+Steiner采高) 或 appendFillVolume/appendLineVolume(方案B,stencil体积)
     └ 点/标注:走符号统一路径(同 MVT 点)
   → uploadBucketGpu → buildRenderCommands
   → VectorFill/VectorLine(depthTest=true) 或 VectorStencil(order 29,ClassifyVolume+ClassifyColor 两阶段)→ 上屏
```

---

## 关键契约与不变量

1. **zoom 三义,裸 int/double 传递,类型上无法区分**:瓦片 zoom(数据瓦自身z,固定)/页 zoom(页存储按屏幕误差选,比直觉高 2-3 档)/相机 zoom(连续)。已踩两次(场封顶设14 滤掉末梢路;宽度 ramp 停点落在可见区间外)。
2. **白名单 vs 逐要素过滤不是同一件事**:`includeLayers` 是整层白名单(空=全收);`layerRules` 是细则,**未列出的层是"全收不过滤"不是"跳过"**。曾代价是画面叠加+54.8ms GPU。
3. **获取归单一层,选择归消费方**:三条消费路选择策略各自独立,但获取必须共享 `MvtTileFetchCache`,否则同一数据瓦拉三次(81ms/巨瓦)。
4. **采样链单一治理点**:`eePageStoreCompose` 是页存储采样+场解算唯一实现,注入六个 shader。历史两次在单条管线漏配特性(合批漏拷场uniform;位移路径喂错UV),症状随管线选择时隐时现极难归因;现有 `tools/check_pipeline_feature_contracts.py` 守卫四类契约。参数已达 11 个,继续加会失控。
5. **渲染固定状态按矢量类型分道校验**:`validateMvpRenderCommands` 对 Fill/Line 强制 `depthTest=true`,对 Point/Label 强制 `false`;违规直接 abort。
6. **worker 边界**:可样式过滤/MVT→Feature/镶嵌/栅格化/场烘焙;**不可碰图集/GL/地形采样器**。锚点存经纬度不存 ECEF。
7. **失败安全是系统性默认**:场纹素 A==0 空哨兵、drape 失败回全透明图、fetch 失败不入缓存、瓦片整瓦原子替换(宁留旧瓦不留半张)、图标名不命中回落 circle。
8. **D2 场编码容量上限（⚠️即将废弃路径）**:偏移范围 4.0 texel、端点余量 1.5 texel,已被模拟证明"调不出更好的解"(P7)；这些约束仅对现有兼容实现有效。
9. **R\* 置换单元原子换手**(点/标注换代):`VectorTileTree::update` 递归置换细化,`renderTiles` 恒为精确覆盖。⚠️ 最初的"全有全无回退"已撤销(2026-08-15,曾致 POI 内容整支降一个数量级),现回到"允许重叠、不允许空洞"。

---

## 诚实得失

### ✅ 强项(有实测背书)
- **表示随负载换掉真实开销（历史证据）**:面栅格化 75ms→~0 GPU(刀1);线 SDF/D2 场把独立 overlay pass 25-30ms 压到 ~0.01ms(刀2);镶嵌峰值 16107ms→216ms(E1)。线场数据保留用于解释为何曾采用该路径，不构成继续扩展承诺。
- **线宽真屏幕像素恒定**,近景/远景/祖先页/页界跳档同级路一样粗(V3),超过 maplibre 平面模式与地形 RTT 模式两种水位(已用 `.ref` 源码核实非口号)。
- **失败安全系统性覆盖**,不是零散补丁。
- **机器可查契约多层并存,真的抓到过 bug**(采样链治理点守卫、MVP 固定状态校验、AI_INDEX 行号守卫、相位打包口径守卫)。
- **量化文化,允许处方被推翻**(P2 记录"量完发现处方错了";P3 记录"原描述『内存涨』是错的,真风险是满后永久丢字")。
- **worker 化彻底**,重活无一在渲染线程。判死清单持续更新,写明死因。

### ⚠️ 短板 / 已知债

架构层面:
- **三条 MVT 消费路没有共同抽象层**,加新数据类型要从头决定走哪条路、从头接线;第四条(如 3D 建筑)出现会立刻暴露。
- **样式系统割裂成四套**(`VectorRasterStyle` / `FeatureRenderStyle`+`StyleExpression` / `SourceLayerRule`+`StyleFilter` / `style/OverlayStyle.h`——后者是旧 `VectorLayer` GeoJSON 路径的平行模型,仍在 demo 上线),没有统一样式模型。且**面/线两路运行期物理改不了**(drape 无 setter、场配置首帧快照),不只是"要改四处"。判据与差距见 **northstar V26**;**建议还债顺序里排最高优先**。
- **线的样式表达力被 D2 场编码卡死**:无图层/分类通道,全图线只能一个颜色 → 卡住多色路网;V9(dash)只被卡住"沿折线弧长"那半个语义(见 northstar V9)。
- **zoom 三义无类型区分**,已踩两次。
- **样式硬编码在 demo 代码里**(`GLESView.cpp` ~90 行 `FeatureRenderStyle` 样式语句),换城市/数据源要改代码重编译;分级规则已抽到 `MinimalGlobeDemoConfig.cpp` 并有守卫(债已部分还)。同属 **V26**。
- **Metal 侧系统性滞后且无守卫**(V20 ❌):场解算 MSL 变体从未真机验证;instanced MSL 精简 uniform 缺场参数;符号 shader 无遮挡判定;守卫只查结构体字段同序,不查特性齐全。
- **测试覆盖偏数据/算法,渲染状态与 shader 缺执行级守卫**:符号遮挡改动 host 188/188 全绿却真机首帧 abort。

性能债(截至 2026-08-16):
- **P3(已量化,暂不修)**:字形图集 16MB 恒定,真风险是容量耗尽后**永久丢字**(缺字而非缺标签,更难察觉)。约 2.7 座城市触顶,已加 80% 告警。
- **P4(告警哨兵已埋)**:placement 时间片增量未做,万级候选会尖刺。
- **P7(已量化,不修)**:D2 单纹素单线容量债——最坏漏画 ≈1.5%(z19 深放大),常用段 ≤0.06%。已否掉"免费"方案(烘焙侧仲裁选亚军实测负收益 9 倍,死因=要素级冗余≠几何级冗余)。
- **P8(不修)**:opacity 回写要重传整条标签顶点流(32B/顶点),maplibre 用独立 1 字节/顶点 buffer(32× 省)。稳态已靠逐 entry 比较免写避开。
- **V25/V20/V9(❌未做)**:标注离场淡出(硬消失无淡出)/ Metal 等价 / 虚线 dash(场编码支持但 FS 未做)。
- **已判死、勿再提**(北极星 E 节):墙带+深度带裁剪单 pass(50-65ms)、独立 overlay pass 画矢量(25-30ms)、面/建筑进 SDF 场(Nyquist 死局)、场解算分母用 `fwidth`/`dFdx`、线宽烘进场纹素。**TBDR 铁律(判死依据):pass 数不是成本,片元数×FS 重量才是**。

---

## 扩展点
- **新 MVT 面图层**(landuse):`makeMvtDrapeStyle()` 加一个 `VectorRasterLayerPaint`,drape 通路自动栅格化进页合成,无新 draw、无新上传路径。
- **新 MVT 线图层：不要再接入道路线场路径（⚠️即将废弃）**。现有 `railway` 等配置仅作兼容维护和回归对照；不得再通过 `makeMvtRoadFieldStyle()` 放行新层，也不要继续扩展场编码、分色通道或 `kMvtRoadFieldMaxZoom`。替代表示待另立设计。
- **新 MVT 点图层**:`mvtOpts.includeLayers` 加层名(⚠️ 白名单契约);样式 `pointImageExpr`/`pointColorExpr` 用 `match("kind",…)` 分类;新图形需 `SymbolShape.h` 加枚举 + `kSymbolSdfBody` 加解析 SDF 分支(GLSL/MSL 共用同一份文本)。
- **新表示类型**(3D 建筑挤出):先回答"它的视觉本质对应哪个物理量",再查 northstar E 节确认没被判死。3D 建筑走 glTF 类通路(V6 已定案为未来内容)。
- **新平台后端(Metal)**:`eePageStoreCompose` MSL 变体已在治理点里但从未真机验证;补齐前不要假设 Metal 与 GLES 等价。
- **新样式表达式**:进 `data/StyleExpression`,`literalString` 必须独立命名;确认是否烘顶点(色类)还是走 uniform(尺寸/宽度类)。

---

## 对照系

| 维度 | maplibre | cesium-native | 本引擎 |
|---|---|---|---|
| 线贴地+像素宽 | 平面模式屏幕空间逐帧画(✅恒定❌无3D);地形RTT模式两次重烘间位图拉伸,重烘指纹**不含zoom** | `GroundPolylinePrimitive`:单pass、FS读全局深度纹理逐fragment重建,像素级贴地+精确像素宽 | D2线段纹素:FS内逐像素解析解算(无插值),寄生地形FS(GPU~0),z封顶后零重烘。**主动不走 Cesium 式 FS 读深度重建**(理由:深度重建/FBO附件/pass顺序三重强耦合恰是本项目最近连续踩坑区域),代价=线宽用墙顶点深度非地形点深度(掠视亚像素偏差)、边缘无羽化 |
| 面贴地 classification | 无对应 | `ClassificationPrimitive`:z-fail双面stencil,与本仓方案B**同构** | 已判死走stencil做真面classification(75ms),改drape栅格化进影像页合成(GPU~0)。**与cesium岔开**——面直接光栅化成像素、丢矢量身份换~0成本 |
| 三分工架构立场 | 全走矢量重画或RTT,不按类型换表示 | 面/线均可走classification、点走billboard——两分工 | **三分工**(面/线/点各换表示),比cesium多切一刀:面走drape而非stencil classification |
| 自研程度 | — | — | D2线段纹素编码是**自研**(否掉标量场/向量场/无限直线三条中间方案) |

**未验证**:cesium ClassificationPrimitive 与本仓 P6a stencil 的逐行对齐仅见于文档描述,本次未直接读 `.ref/cesiumjs` 源码核对;maplibre RTT 指纹出处为 northstar 既有引用,本次未重核行号。
