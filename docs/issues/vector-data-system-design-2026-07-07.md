# 矢量数据系统设计 (Vector Data System)

**日期**: 2026-07-07
**状态**: 设计中 — `.ref` 四路调研已完成(cesium-js / cesium-native / maplibre-gl-js / osgearth 本地分析),引擎关键事实已核实。**已按"编辑=核心能力 + 大量数据规模"重塑重心**(2026-07-07 第二轮)。**三条核心决策已锁**(第三轮):①持久化=纯本地(GeoJSON/GeoPackage 导入导出,无后端同步) ②贴地线/面终态=stencil 分类 B(编辑手柄常驻 A) ③规模=接口留百万级视口分页(load+evict)口,先按万级全驻内存实现。未动代码。剩 §15 三项(MVT 底图优先级/样式来源/下一步)。
**目标**: 建一套**可编辑的大规模矢量数据系统**。数据轴有两类源:①**权威可编辑要素存储**(FeatureStore,你的数据,全 CRUD,万级+,一等公民) ②**只读流式 MVT**(外部底图,显示 + snap 参考)。渲染轴交付四能力:扎实几何(屏幕线宽/join/cap)、贴地形、文字标注(SDF+避让)、marker+聚合。两类源**共用**下游渲染/贴地/标注/拾取。
**前提**: 本文档负责**把架构分层、数据流、模块边界、每个选型的潜在问题在纸上锁死**。三个难点写不清就不该动代码:§7 贴地、§8 标注避让、**§9 大规模可编辑存储(编辑成核心后新增的头号难点)**。工时按 AI 协作基准估,选型按长期架构债判。

---

## 0. 重心说明:为什么编辑改变了一切

用户确认**编辑是核心能力,数据规模万级+**。这把 MVT 流式显示从"主角"降为"只读底图配角",**FeatureStore 成为系统中心**。关键认知:

- **MVT 天生不可编辑**:瓦片边界裁剪(要素碎片化)、坐标量化(extent 4096 有损)、无跨瓦片稳定 ID。拿它当可编辑存储是死路。
- **两种源、两种可变性、一条下游**:`FeatureStore`(可变) + `VectorTileSource`(只读)是两种数据源,但 bucketing/渲染/贴地/标注/拾取**对数据可不可变无所谓**,共用。编辑只碰 FeatureStore。
- **现有 `GeoFeature`(全精度 cartographic rings + 稳定 id)正是可编辑模型的雏形**,不像 MVT。FeatureStore = 把 `GeoFeature` 提升为一等索引化存储,不是新造。
- **"大量可编辑"是新增的头号难点**:万级下不能每次编辑全重镶嵌、编辑要空间查询、编辑要可逆+可回写。这三点 MVT 只读显示路径完全没有。见 §9/§10。

---

## 1. 现状:已有的种子(不是从零)

| 模块 | 文件 | 现状 | 重塑后定位 |
|---|---|---|---|
| GeoJSON 解析 | `data/GeoJsonParser.{h,cpp}` | 整文件 → `std::vector<GeoFeature>`,坐标转 radian,Multi* 分解 | ✅ FeatureStore 的**导入器之一** |
| 要素结构 | `data/GeoJsonParser.h` `GeoFeature` | 全精度 cartographic rings + 稳定 id + 属性表 + bounds | ✅ **可编辑模型雏形**,提升为 `Feature` 存进 FeatureStore |
| 矢量图层 | `layers/VectorLayer.{h,cpp}` | 整文件主线程逐 feature tessellate,per-feature 缓存,`frameBuffers_` **每帧重建** | ⚠️ 标注级种子,拆解:数据入 FeatureStore,渲染入新管线 |
| 样式 | `style/OverlayStyle.h` | Point/Line/Polygon/Interaction,`AltitudeMode` 枚举 | ⚠️ `ClampToGround` 只投椭球面**未贴地形**;保留给简单场景 |
| 渲染命令 | `renderer/RenderCommand.h` | `VectorOverlay` kind(order 30) + colorShader | ⚠️ 只够单色,需扩 kind + shader |
| 坐标 | `core/geodesy/{Ellipsoid,Transforms}.h` | cartographic↔ECEF、ENU、测地法线、射线求交 | ✅ 完备 |
| 地形高度查询 | `tiling/LoadedTerrainHeightSampler.h` | `sampleHeightOptional(tiles,lng,lat)`,`nullopt`=无数据 | ✅ 贴地(点/编辑手柄)复用;需加空间索引 |
| 瓦片框架 | `tiling/{Tileset,TilesetTile}.h` | quadtree 选择/LOD/缓存,`ContentProvider` 模式 | ✅ 只读 MVT 树可平行复刻;可编辑存储**不用**裁剪式瓦片树(见 §9) |
| 拾取射线 | `core/math/IntersectionTests` | 射线/体求交 | ✅ 拾取/snap 复用 |

**现有线镶嵌本质问题**:`tessellateLine` 是"沿椭球面弧分段"的裸折线,`LineStyle.width` 声明像素但未实现。无屏幕线宽/join/cap/dash/贴地/分块/worker/标注/聚合/拾取/编辑。

---

## 2. 参考项目基准(`.ref/` 本地分析结论)

| 关切点 | 权威参考 | 核心做法 | 对本引擎结论 |
|---|---|---|---|
| **MVT 解码** | osgearth `MVT.cpp` + maplibre | varint+zigzag,extent 4096,环用有向面积定外环/孔 | protobuf-cpp 自实现,单测对拍 |
| **瓦片调度/overzoom** | maplibre `vector_tile_source.ts` | `OverscaledTileID`,超 maxzoom 载父瓦片切片 | 只读 MVT 复刻 `Tileset` quadtree |
| **worker bucketing** | maplibre `worker_tile.ts` | 按 layout-family 分组,off-thread 镶嵌 | 复用既有 `ThreadPool` + clip-worker 模式 |
| **线镶嵌** | maplibre LineBucket | `a_pos/a_normal/a_extrusion`,**线宽在顶点着色器展开**,miter/bevel/round | 直接照搬(§6.2) |
| **面三角化** | maplibre FillBucket | earcut,fill-outline 第二索引缓冲 | earcut.hpp |
| **贴地形** | cesium `ClassificationPrimitive` / osgearth `ClampingTechnique` | stencil 阴影体分类 vs GPU-depth-RTT vs 高程采样 vs draping | **RTT 不可用**,排序重塑(§7) |
| **文字标注** | maplibre `symbol/*` | SDF 图集 + shaping + 屏幕空间 collision + 逐帧 placement + fade + 跨瓦片去重 | collision/dedup 需球面化(§8) |
| **拾取/空间索引** | maplibre `query_features.ts` | 每瓦片 feature 空间栅格,先粗筛 cell 再精测 | R-tree/grid,ECEF 射线求交(§10) |
| **可编辑要素模型** | cesium-js `GeoJsonDataSource`/Entity;osgearth `FeatureSource` | 全精度 entity/feature + 属性绑定 | FeatureStore 提升 `GeoFeature`(§9) |
| **样式** | maplibre style-spec | data-driven paint 表达式 | 定义子集(§12) |

**关键否定**:cesium-**native** 无矢量能力(GeoJSON 走外部 `CesiumVectorData` 转 glTF)。矢量/贴地/编辑在本 cesium 移植库是**全新地盘**,基准取 cesium-js + maplibre + osgearth。

---

## 3. 目标架构总览

**FeatureStore 为中心**,分层如下:

```
┌──────────────────────────────────────────────────────────────────┐
│  VectorLayer (公开层) — 图层生命周期/可见性/样式/编辑模式切换        │
├──────────────────────────────────────────────────────────────────┤
│  【数据源轴】                                                       │
│  ┌─ FeatureStore (权威·可编辑·一等)          ┌─ VectorTileSource   │
│  │   全精度 Feature + 稳定 ID + 属性表        │   只读流式 MVT       │
│  │   R-tree 空间索引 + 空间分桶(非裁剪)       │   (外部底图/snap 参考)│
│  │   EditSession: 命令/undo-redo/snap/脏区    │   quadtree + overzoom │
│  └─────────────────────┬────────────────────┴──────────┬──────────┤
│                        ↓ 视口 materialize + 脏区增量     ↓          │
│  【Worker】 Decode/Simplify → Bucketize → Tessellate → SpatialIndex │
│      LineBucket | FillBucket | SymbolBucket                         │
├──────────────────────────────────────────────────────────────────┤
│  VectorRenderContent — GPU 缓冲 + draw command 缓存(按桶/瓦片)      │
├──────────────────────────────────────────────────────────────────┤
│  【共用子系统】 ClampToGround | LabelPlacement | Clustering | Picking│
│                EditHandleRenderer (编辑手柄/gizmo)                  │
├──────────────────────────────────────────────────────────────────┤
│  RenderCommand 新 kind + 新 shader (fill/line/sdf/classify/handle)  │
├──────────────────────────────────────────────────────────────────┤
│  【持久化】 GeoJSON/GeoPackage 导入导出 (纯本地,无后端同步)         │
└──────────────────────────────────────────────────────────────────┘
```

**两条数据流**:

*只读 MVT(显示底图)*:
```
视口 → MVT 树 select → 请求 .pbf → [worker] 解码+bucketize → 上传 → 渲染
```

*可编辑要素(你的数据,核心)*:
```
导入 GeoJSON → FeatureStore(R-tree 索引 + 分桶)
  视口变化 → 空间查询可见桶 → [worker] 简化+镶嵌 → 上传 → 渲染
  编辑操作 → EditSession 命令 → 改 Feature + 标脏桶 → [worker] 只重镶嵌脏桶 → 上传
  拖拽中 → 编辑手柄实时贴地(方案A) + snap 查询 R-tree
```

**设计原则**:
1. **FeatureStore 是唯一真相源**,GPU 缓冲是它的**派生**。编辑改存储,派生随脏区增量重建。
2. **分桶 ≠ 裁剪**。可编辑数据按空间分桶做镶嵌单元,但**桶按要素 ID 归属整个要素,不在桶边界裁**(裁了就没法编辑跨桶要素)。这是与 MVT 瓦片的本质区别。
3. **重活全下 worker**(解码/简化/镶嵌);主线程只做上传 + 每帧轻活(placement/clamp uniform/手柄)。复用 `clip-worker-ization-done` 快照+认领模式。
4. **平行扩展不改现有语义**(手术式):除 3 个文件加字段,其余全新增。

---

## 4. 数据源抽象

```cpp
// 两种源共用的下游契约:都产出"可镶嵌的要素批"
class VectorFeatureSource {
public:
    virtual bool isEditable() const = 0;
    virtual std::vector<FeatureRef> queryVisible(const Frustum&) = 0;  // 视口可见要素
};

class FeatureStore : public VectorFeatureSource {   // 权威可编辑,§9
    /* R-tree + 分桶 + Feature 全精度 + 属性表 + EditSession */
};

class VectorTileSource : public VectorFeatureSource {  // 只读 MVT,§5
    bool isEditable() const override { return false; }
    /* quadtree 树 + MvtDecoder + overzoom */
};
```
- **导入**:`GeoJsonImporter`(包现有 `GeoJsonParser`)、未来 GeoPackage/Shapefile → 灌进 FeatureStore。
- **MVT 源**:`{z}/{x}/{y}.pbf` 走既有 `CurlScheduler`(CA blob 已修,HTTPS 可用)。
- **snap 互通**:编辑时可 snap 到只读 MVT 要素(参考几何),故 `queryVisible` 统一契约。

---

## 5. 只读 MVT 管线(底图,非核心但要有)

- **`MvtDecoder`**(worker):protobuf-cpp → 增量解码(zigzag+delta,extent 4096)→ 环有向面积分外环/孔。单测对拍 osgearth `MVT.cpp`。
- **`VectorTileTree`**:结构复刻 `tiling/Tileset` 的 quadtree 选择/SSE/kick,**独立实例**(独立 maxzoom/loadQueue)。不绑地形树(LOD 语义会互污,cesium 亦各自成树)。
- **overzoom**:超 maxzoom 载父瓦片,对已解码几何 scale+offset+clip,缓存切片。
- **潜在问题**:量化精度(z14 够/深缩放放大误差)、双树选择开销(矢量用更松 SSE + `cullRequestsWhileMoving`)、LRU 缓存移动端保守 200–400。

---

## 6. 几何渲染(两源共用)

### 6.1 RenderCommand 扩展
| 新 kind | shader | 用途 |
|---|---|---|
| `VectorFill` | `kVectorFill*` | 面填充(data-driven 色) |
| `VectorLine` | `kVectorLine*` | 屏幕空间线宽 + join/cap |
| `VectorSymbol` | `kVectorSymbol*` | SDF 文字 + 图标 |
| `VectorClassify`(可选) | `kVectorClassify*` | stencil 贴地(§7) |
| `EditHandle` | `kEditHandle*` | 编辑顶点手柄/gizmo(§10) |

shader 走 `Renderer.cpp` 内嵌 MSL+GLES 双份约定。

### 6.2 线镶嵌(照搬 maplibre)
`a_pos(ECEF 相对桶原点) + a_normal + a_extrusion`,**线宽顶点着色器展开**:
```glsl
vec4 clip = u_mvp * vec4(a_pos, 1.0);
vec2 offset = a_normal * u_lineWidthPx * clip.w / u_viewportPx;
gl_Position = clip + vec4(offset, 0.0, 0.0);
```
join: miter/bevel/round;cap: butt/square/round;dash: 弧长参数 fragment mod 丢弃。worker 生成。

### 6.3 面 + 抗锯齿
earcut 三角化 + fill;outline 第二索引缓冲。**无 MSAA framebuffer** → fragment 边缘 alpha(distance-to-edge)柔化,不依赖 RTT。

### 6.4 潜在问题
球面屏幕线宽近地平线畸变(clamp 最大挤出);贴地深度(§7);半透明排序复用 `translucentSortDepth`;顶点绑桶原点避免 double→float 截断(对齐 `android-demo-env-gotchas`)。

---

## 7. 贴地形 Clamp-to-Ground(难点一)

### 7.1 引擎约束(已核实,重塑方案排序)
- ❌ `createFramebuffer` 直接 `return nullptr`("MVP不需要")→ **RTT 全线不可用** → osgEarth draping / GPU-depth-RTT 钳制**封死**(除非先实现 framebuffer,独立大工程)。
- ✅ `LoadedTerrainHeightSampler::sampleHeightOptional` 已存在 → 高程采样贴地可复用。
- ⚠️ stencil 未暴露(后端仅深度三态,`RenderCommand` 无 stencil 字段)→ cesium 分类需扩渲染状态,但**不需 RTT**。

### 7.2 三条可行路(RTT 已排除)

| 方案 | 原理 | 精度 | LOD切换时 | 编辑友好度 | 工程 |
|---|---|---|---|---|---|
| **A. 高程采样(CPU顶点偏移)** | worker 对每顶点查 `sampleHeightOptional` | 受地形网格插值限,边界台阶 | ❌ 需重钳 | ✅ **有CPU几何+高度,拖拽实时反馈** | 小 |
| **B. stencil 阴影体分类** | 几何挤成体,stencil pass,color 只画 stencil≠0,关深度测试 | 像素级 | ✅ 采样已渲染深度天然正确 | ⚠️ 无CPU几何,编辑手柄仍需A | 中 |
| **C. GPU高程shader钳制** | 顶点属性带anchor+height,着色器采高程层 | 受高程层LOD限 | ⚠️ 高程层可能滞后 | ⚠️ | 中 |

### 7.3 推荐:分而治之(编辑场景强化了这个分法)
- **点/marker/标注锚点/编辑手柄** → **方案 A(高程采样)**。数量少、采样器现成、**拖顶点实时贴地反馈只能靠 A**(有 CPU 几何+高度)。今天能落地。
- **密集线/面(提交后的静态几何)** → **方案 B(stencil 分类)为终态**。globe 上唯一"地形 LOD 切换仍正确+像素精确+不依赖 RTT"。stencil 落地前线面暂走 A(过渡态,非长期妥协)。
- **depth-offset**:A 方案贴地线防 z-fighting 需要。

### 7.4 潜在问题
- **方案 A 查询成本**:采样器现为 O(瓦片×三角形)(`camera-terrain-height-perf` 优化过但无空间索引)。密集顶点/编辑实时查询会放大 → **必须给采样器加 BVH 空间索引**(§9 的存储索引可复用思路)。
- **方案 B stencil 扩展面**:影响所有后端 PSO,需复核 `render-backend-winding-contract`(Metal CW/GLES CCW);挤出方向依赖测地法线。
- **无数据区**:nullopt 回退椭球面(对齐 `no-fine-data-ellipsoid-fallback`)。

---

## 8. 文字标注 Labels(难点二)

### 8.1 静态部分(worker,照搬 maplibre)
SDF glyph 图集(range PBF + potpack)、shaping(断行/定位/对齐)、icon 图集、anchor 生成(point/line/line-center/polygon pole-of-inaccessibility)、collision box。**CJK/中文**需字体 fallback stack + 可选竖排。

### 8.2 动态部分:球面化逐帧避让
maplibre placement 是屏幕空间,8 处 2D 假设在 globe 失效,必须改造:collision grid 保留但加**视锥+地平线+地形遮挡剔除**;跨瓦片去重改**地理 key**(lng/lat 量化);尺寸按每符号 3D 距离;线锚点测地弧长;近地平线 fade-out。三段 placement(collect→place→commit,fade ~300ms)。

### 8.3 潜在问题
全系统最难部分。建议独立分期:先"无避让全画"验证 SDF,再上 placement。地平线遮挡复用现有设施(避 `occlusion-zero-horizon-point-patchwork` 的 (0,0,0) 哨兵坑)。**编辑联动**:选中要素的标签应优先显示(placement priority 提权)。

---

## 9. 大规模可编辑要素存储 FeatureStore(难点三·编辑核心)

### 9.1 权威存储
```cpp
struct Feature {                    // GeoFeature 的一等提升
    FeatureId id;                   // 稳定全局 ID
    GeometryType type;
    std::vector<std::vector<Cartographic>> rings;  // 全精度 double,不量化不裁剪
    PropertyMap properties;
    Rectangle bounds;
    uint64_t version;               // 服务 undo/redo(纯本地,无后端同步)
};
class FeatureStore {
    std::unordered_map<FeatureId, Feature> features_;   // 权威
    RTree spatialIndex_;                                 // bounds → FeatureId
    std::unordered_map<BucketKey, Bucket> buckets_;      // 空间分桶(镶嵌单元)
};
```

### 9.2 分桶(≠ 裁剪)——万级规模的关键
- 按空间网格(如固定经纬格 或 自适应 quadtree cell)把要素分桶,**桶是镶嵌/上传/脏区单元**。
- **要素按 ID 整体归属一个主桶**(用 bounds 中心定桶),**不在桶边界裁几何**——跨桶大要素完整存在主桶,渲染时整体画。裁了就无法编辑一个完整要素。
- 视口驱动 **materialize**:R-tree 查可见桶 → 只镶嵌可见桶 → 上传。远处桶只在索引里,不占 GPU。
- **LOD 简化**:密集要素按 zoom 做几何简化(Douglas-Peucker,worker),远缩放少顶点。简化结果按 zoom 缓存。

### 9.3 编辑增量重镶嵌
- 编辑一个要素 → 定位其所属桶 → **标脏** → worker 只重镶嵌该桶(+ 若 bounds 变化跨桶则迁桶) → 增量上传。**绝不全量重镶嵌**。
- 拖拽中(高频):不重镶嵌整桶,单独走"编辑预览"轻量 buffer(仅被拖要素),松手才 commit 进桶重镶嵌。

### 9.4 规模策略(已锁)
**接口留百万级视口分页口,先按万级全驻内存实现。** 具体:
- `FeatureStore` 接口**不预设全驻内存**:`queryVisible` 返回**异步**(可触发分页 load),桶有 `Resident/Evicted` 状态,远处桶可 evict 只留 R-tree 里的 bounds+ID。
- **P0 实现按万级全驻内存**(所有 Feature + 桶常驻),但接口签名(async query + 桶生命周期)已按分页设计 → 未来接 GeoPackage/后端分页无需改接口。**这是接口决策,不是妥协**。
- 分页数据源(百万级)后置,可从本地 GeoPackage(SQLite+R*Tree)按视口 bbox 查询增量 load。

### 9.5 潜在问题
- **桶粒度**:太大→编辑重镶嵌慢;太小→桶数爆炸+跨桶要素多。需按要素密度自适应(参考 R-tree 节点容量)。
- **跨桶大要素**:一条洲际线 bounds 巨大 → 单桶画整条,视口剔除失效。极大要素单独走"大要素桶"不参与网格分桶。

---

## 10. 编辑交互:操作 / undo-redo / snap / 手柄(编辑核心)

### 10.1 EditSession(命令模式)
```cpp
struct EditCommand { virtual void apply(FeatureStore&); virtual void undo(FeatureStore&); };
// MoveVertex / InsertVertex / DeleteVertex / MoveFeature / AddFeature / DeleteFeature / EditAttribute
class EditSession {
    std::vector<std::unique_ptr<EditCommand>> undoStack_, redoStack_;
    void execute(cmd);  // apply + push undo + 标脏桶
    void undo(); void redo();
};
```
每条命令 apply/undo 都改 Feature + 标脏 + 记 version。撤销 = 反向 apply + 重镶嵌脏桶。

### 10.2 Snap 吸附
拖顶点时查 R-tree 找屏幕阈值内的**顶点/边**(含只读 MVT 参考几何)→ 吸附。需 ECEF 距离 + 屏幕投影阈值换算。

### 10.3 编辑手柄渲染
选中要素的顶点渲染为可拖 `EditHandle`(billboard 点,走方案 A 实时贴地)。中点手柄(插顶点)、旋转/缩放 gizmo(面/多点)。手柄拾取复用 §10.4。

### 10.4 拾取
每桶 worker 建 feature grid(cell→featureID[]);屏幕点 → ECEF 射线(`IntersectionTests`)→ 命中桶 → 查 cell → 精测(点距/线距/面包含)→ render order 排序。复用 `setFeatureState`(hover/selected,已存在)不重建几何。

### 10.5 潜在问题
- **高频拖拽性能**:拖拽每帧重投影+snap 查询,须只碰被拖要素 + R-tree 局部查询,不全量。
- **贴地要素的编辑**:方案 B(stencil)无 CPU 几何 → 编辑时临时切方案 A 拿几何,commit 后回 B。
- **多选/框选**:R-tree 范围查询。

---

## 11. 图标/Marker + 聚合

marker 走 `SymbolBucket` icon 路径。聚合:层级预聚(worker,supercluster 式,稳定不闪),渲染按 zoom 取层。**聚合与桶边界冲突** → 聚合在 FeatureStore 全局做(有全量索引),不受桶/瓦片边界限(优于 MVT 流式聚合)。**编辑与聚合互斥**:进编辑模式展开聚合(要能点到单个要素)。

---

## 12. 样式系统

MapLibre style-spec 子集:`layers[]{type,source,source-layer,filter,layout,paint}`。data-driven paint 表达式(`get`/`case`/`interpolate`)worker 求值 → per-feature 顶点属性 或 uniform。现有 `OverlayStyle` 保留给简单场景内部转换。**先字面量+zoom 函数,表达式后续**。

---

## 13. 与现有引擎集成点(具体文件)

| 集成点 | 现有文件 | 改动 |
|---|---|---|
| 渲染命令 | `renderer/RenderCommand.h` | 加 Vector*/EditHandle kind + order;stencil 字段(方案B) |
| shader | `renderer/Renderer.cpp` | 内嵌 5 组 MSL+GLES + accessor(仿 `colorShader()`) |
| 要素结构 | `data/GeoJsonParser.h` | `GeoFeature` 提升为 `Feature`(加 version) |
| MVT 树 | `tiling/Tileset.*` | **复刻**为 `VectorTileTree`(不改 Tileset) |
| 内容状态 | `tiling/TileRenderContentState.h` | 平行加 `VectorRenderContent` |
| worker | 既有 `ThreadPool` | 复用 + 优先级/budget lane |
| 上传 | 既有 upload budget | 走同一 finalize 预算(`raster-upload-interaction-fixed` 涓流语义) |
| 高度查询 | `tiling/LoadedTerrainHeightSampler.h` | 贴地/编辑手柄复用;**必须加空间索引**(密集查询) |
| HTTP | 既有 `CurlScheduler` | MVT 源复用 |
| 场景装配 | `scene/Scene.*` `layers/` | `VectorLayer` 注册 + 编辑输入路由 |
| 拾取/snap | `core/math/IntersectionTests` | 复用 |

手术式:除 `RenderCommand.h`/`Renderer.cpp`/`TileRenderContentState.h`/`GeoJsonParser.h` 加字段(平行扩展不改语义),其余全新增。不动地形/raster 现有路径。

---

## 14. 落地阶段(工时 AI 协作基准,质量优先)

> 分期是实现顺序,非"MVP妥协→V2重构"。过渡实现(如线面暂走高程采样)明确标注。**编辑成核心后,FeatureStore + 编辑前移到早期**。

| 阶段 | 内容 | 可验证目标 |
|---|---|---|
| **P0** | FeatureStore 骨架(R-tree + 分桶)+ GeoJSON 导入 + `Feature` 提升 | 导入万级要素,视口空间查询正确(单测) |
| **P1** | FillBucket + LineBucket + 新 shader,视口 materialize 渲染(非贴地) | 真机画出万级要素,视口驱动只镶可见桶 |
| **P2** | EditSession(命令/undo-redo)+ 拾取 + 编辑手柄 + snap + 脏区增量重镶嵌 | 拖顶点/加删要素/撤销重做,只重镶脏桶,万级流畅 |
| **P3** | 贴地方案 A(点/手柄/线面过渡)+ 采样器空间索引 | 要素贴地形,拖顶点实时贴地反馈 |
| **P4** | 只读 MVT 底图(`VectorTileTree` + `MvtDecoder`) | MVT 底图显示,可 snap 到 MVT 参考 |
| **P5** | SDF 标注(无避让→球面化 placement 避让) | 文字显示 + 不重叠 + 地平线 fade |
| **P6** | stencil 贴地终态 + 聚合 + data-driven 样式 + GeoPackage 导入导出 | 线面 LOD 切换像素贴合;聚合;表达式;本地存取 |

**关键路径**:P0→P1→P2 是可编辑基座(编辑核心,优先);P3 贴地、P5 标注、P6 stencil 是独立难点可并行。MVT 底图(P4)后置——它是配角。

---

## 15. 决策状态

**已锁(第三轮)**:
1. ✅ **持久化 = 纯本地**。GeoJSON/GeoPackage 导入导出,**无后端同步**。→ §10.1 `version` 仅服务 undo/redo,不建脏队列/冲突机制;P6 移除后端同步。
2. ✅ **规模 = 接口留百万级视口分页口,先按万级全驻内存实现**。→ §9.4。
3. ✅ **贴地线/面终态 = stencil 分类 B**,编辑手柄常驻 A,stencil 落地前线面走 A 过渡。→ §7.3。接受跨 Metal/GLES 扩展 stencil 渲染状态契约的改动面。

**剩余(建议默认,你不反对我就照此推进)**:
4. **MVT 底图优先级**:默认 **P4 后置**(先把可编辑核心 P0-P2 做扎实,底图是配角)。若你要先有底图垫背景才好验证编辑体验,再前移。
5. **样式来源**:默认**自定义精简样式子集先行**(字面量 + zoom 函数),标准 MapLibre style.json + 表达式引擎后置。
6. **下一步**:P0-P2 可编辑核心(导入→渲染→编辑/undo/snap)vs 继续细化 §9/§10 实现细节 —— **等你一句话定向**(见对话)。

---

## 附:已排除/已证伪(勿重查)
- **RTT draping / GPU-depth-RTT 钳制**:`createFramebuffer` 返 nullptr,RTT 不可用,已排除。
- **MVT 当可编辑存储**:瓦片裁剪+量化+无稳定 ID,天生不可编辑,已排除。可编辑走 FeatureStore。
- **分桶时裁剪要素**:裁了无法编辑完整要素,已排除。桶按 ID 整体归属不裁。
- **绑地形 Tileset 复用瓦片树**(只读MVT):污染 LOD 语义,不绑。
- **就地改 VectorLayer 升级**:标注级种子,数据入 FeatureStore、渲染入新管线,不就地改。
- **cesium-native 矢量惯例**:native 无矢量能力,基准取 cesium-js + maplibre + osgearth。
