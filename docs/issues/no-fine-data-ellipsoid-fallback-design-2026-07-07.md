# 无细数据区回落椭球 — 设计（2026-07-07）

目标:消除**部分覆盖地形数据集**下、无细数据区域暴露的巨型 skirt 裙墙 / 粗 QM 台阶
(梯度),让未覆盖区回落为平滑椭球面,同时保持覆盖区真实地形。

---

## 0. 先决:确认触发条件(重要,先验证再动手)

这个 artifact **只在真·部分覆盖数据集下出现**,全局数据集不触发:

- **Cesium ion World Terrain(当前默认 `kUseCesiumIonTerrain=true`)是全局覆盖**——全球每个瓦
  片至少在中低 zoom 可用(海洋=近椭球数据),`layer.json` availability 覆盖全球。所以"某瓦片
  4 个子全不可用 → 粗叶子 + 385km 裙墙"这条链在 ion 下**根本不会发生**。
- 历史上暴露此问题的是**本地 8090 只有四川的 QM 服务器**(`kUseCesiumIonTerrain=false`)。

**行动前必须先确认**:当前真机看到的梯度/破碎是跑在 ion 还是局部数据集?
- 若在 **ion(全局)**:椭球回落**不会改变任何东西**(无未覆盖区)→ 梯度另有其因(更可能是
  LOD cross-fade 未开的硬切、或影像色调镶嵌),此设计不该是首选。
- 若在**局部数据集**、或未来要支持任意第三方 partial QM:此设计正是解药。

结论:椭球回落是**支持任意 partial 数据集的正确性/健壮性基建**,在全局数据集下是**惰性零改
动**的安全网。是否现在落地取决于你是否需要跑 partial 数据。

---

## 1. 根因链(代码级)

以 cesium replace-refine 地形为参照(`.ref/cesium-native/Cesium3DTilesSelection/src/LayerJsonTerrainLoader.cpp:1071-1103`):

细化一个瓦片时,看 4 个子的 availability:
- **任一子可用 → 建全部 4 子**:可用子 = `QuadtreeTileID`(加载真 QM);不可用子 =
  `UpsampledQuadtreeNode`(裁剪父几何 = 平滑续延,小 skirt)。
- **4 子全不可用 → `return {}`(叶子)**:细化停在当前层。

我们的 [TileChildMaterializer.h:82-90](../../scaffold/src/earth_engine/tiling/TileChildMaterializer.h)
`if (!anyChildAvailable) return false;` **与之逐字一致**(非 bug)。

**裙墙来源**:在 partial 数据集,广大区域 availability 为空 → 瓦片在很粗的层(极端是 z0)就 4 子
全不可用 → 停成粗叶子。粗叶子 skirt 高 = `5·maxGeometricError·width`
([QuadtreeGeometricError.cpp:14-23](../../scaffold/src/earth_engine/tiling/QuadtreeGeometricError.cpp)),
z0 tile width≈π → skirt≈**385km**。均匀粗区里 skirt 塞在同级邻居下不可见;但在**覆盖区(细)与
未覆盖区(粗叶子)交界**,粗叶子的巨墙无同级邻居遮挡 → 暴露成同心环台阶(梯度)。

关键洞察:**cesium 靠"全局 availability"回避此问题**——ion 全球都有数据,永不出现 z0 全空叶子。
我们要在 partial 数据集下复刻这个"全球可用"性质,用**椭球**回填空洞。

---

## 2. 设计目标 / 非目标

**目标**
- 未覆盖区渲染平滑椭球面(h=0 或参考海平面),而非粗 QM 台阶 + 巨型裙墙。
- 全局数据集下**零改动、零回归**(惰性)。
- 覆盖区真实地形不受影响。
- 与 selector/golden 对拍保持可控(新增几何是 additive,不动选择算法)。

**非目标(诚实边界)**
- **不**试图隐藏覆盖区与椭球区之间的**真实高度悬崖**(如四川高原 +4km 接海平面 0)。4km 落差
  压缩到一个瓦片宽本就是一堵墙——这是数据的诚实表达,不是渲染 bug。设计只消除**人造的
  385km 裙墙**和**粗 QM 台阶**,把未覆盖区变成干净椭球。
- **不**做 geomorph 高度渐变(那是更大工程,除非悬崖碍眼)。

---

## 3. 方案对比

### 方案 A(推荐)— 全局椭球 availability backfill(composite provider)

**思路**:给地形 provider 一个**全局椭球可用性地板**——任何 QM 无数据的瓦片,availability 返回
"可用",内容由 `EllipsoidTerrainContentProvider`(已存在的平滑网格生成器)提供。于是:
- `anyChildAvailable` **永远为真**(椭球兜底)→ 永不出现"粗叶子";
- 细化由 SSE 正常驱动,未覆盖区得到**合适 LOD 的平滑椭球瓦片**(小 skirt,同级邻居互相遮挡);
- 覆盖区照常走 QM。

**落点**:新增 `CompositeTerrainProvider`(包装 QM provider + `EllipsoidTerrainContentProvider`):
- `availabilityState(key)`:QM 可用 → Available(源=QM);否则 → Available(源=Ellipsoid)。**永不
  NotAvailable**(除非超 maxZoom)。
- `requestTileContent(key)`:按源路由——QM 源走 QM 加载;Ellipsoid 源走椭球网格生成。
- `rootTiles()` / `childTiles()`:并集(实际由椭球全局网格决定,QM 是子集)。
- 每瓦片记录"源"(QM / Ellipsoid),供 `TileChildMaterializer` 与 upsample 逻辑区分。

**优点**:最贴近 cesium 的"全局覆盖"本质;未覆盖区是正常 LOD 平滑椭球(非单块粗叶子);全局数据
集下椭球源永不命中 → 零改动。
**缺点**:引入 provider 组合层 + 每瓦片"源"标记;椭球瓦片也要建 skirt(小)以防椭球瓦片间裂缝。

### 方案 B(轻量替代)— 就地 no-data 叶子椭球化

**思路**:保留单 QM provider。在 `TileChildMaterializer` 命中"4 子全不可用"且当前瓦片是 no-data
时,**不**停成粗 QM 叶子,而是:①把该瓦片内容替换为椭球网格;②允许再细化几层(让椭球面有合理
镶嵌 + 小 skirt),或直接以椭球叶子渲染但**去掉巨型 skirt**。

**优点**:改动局部,不引入 provider 组合;**缺点**:"再细化几层"的停止条件要另定(否则椭球区无限
细化);与 upsample 语义交织,边界 case 多;不如 A 干净。

### 方案 C(不推荐)— skirt 封顶

只把暴露边缘的 skirt 高度封顶到"与邻居高差"。**缺点**:skirt 本职是遮裂缝,封顶会重新引入裂缝;
且不解决粗 QM 几何本身的台阶。仅治标,**否决**。

---

## 4. 推荐方案 A 的详细机制

### 4.1 Availability 地板
```
CompositeTerrainProvider::availabilityState(key):
    if key.z > maxZoom: return NotAvailable
    if qm_->availabilityState(key) == Available: return Available   // 源=QM
    return Available                                                // 源=Ellipsoid(兜底)
```
效果:`materializeTerrainChildren` 的 `anyChildAvailable` 恒真 → 永不粗叶子 → 无暴露巨墙。

### 4.2 内容路由 + 源标记
- 每 `TilesetTile` 记录 `terrainSource ∈ {Qm, Ellipsoid}`(新增字段或复用 content kind)。
- `requestTileContent`:QM 源 → 现有 QM 加载路径;Ellipsoid 源 → `EllipsoidTerrainContentProvider`
  生成 gridSize×gridSize 平滑网格(h=0)。
- **过渡瓦片**:一个 QM 瓦片的某子无 QM 数据但父有 → 现状是 QM upsample(裁剪父几何,平滑,保
  留真实高度)。**这条保持不变**(cesium 行为),只有在**父也无 QM**(纯未覆盖区)才走椭球。即:
  椭球只在"连父都没有 QM"的纯空洞区生效,覆盖区边缘仍靠 QM upsample 平滑续延。

### 4.3 椭球瓦片的 skirt
椭球网格瓦片之间也需小 skirt 防裂缝(同 QM,`5·GE·width`,但因未覆盖区 LOD 通常较粗,skirt 仍有
一定高度)。同级椭球邻居互相遮挡 skirt,不暴露。

### 4.4 覆盖/未覆盖边界(诚实悬崖)
- 覆盖区最深 QM 瓦片(高度 H)与相邻椭球瓦片(高度 0)之间有 H 的垂直落差。
- QM 瓦片自身 skirt 从边缘 H 向下垂,椭球瓦片 skirt 从 0 向下垂 → 二者之间留 H 的缝。
- **处理**:①若 H 落差 ≤ 数据边缘 skirt 高,skirt 覆盖,无缝;②若 H 是真陡崖(四川高原边),让
  **椭球侧边界瓦片的 skirt 向上补齐到覆盖侧边缘高度**(边界特判:椭球瓦片邻接 QM 瓦片时,skirt
  顶抬到邻居边缘高)——或接受诚实垂直墙(4km 落差本就该是墙)。**默认接受诚实墙**,除非碍眼再做
  边界 skirt 抬升(§7 P2)。

### 4.5 与 selector / golden 的关系
- 椭球回落是**内容层**改动(哪个 provider 供几何),**不动**选择/剔除/SSE 算法。
- golden 对拍跑在固定数据集,若该数据集全局(默认)→ 椭球源不命中 → golden 字节不变。
- 若给 golden 加 partial 数据集场景,需新 golden 基线(明确、非豁免)。

---

## 5. 分期落地(目标驱动,每步可验证)

**P0 先验证触发**:真机确认当前是否在 partial 数据集出现裙墙;若在 ion 全局且仍有梯度,先查
cross-fade/影像(此设计不适用)。验证信号:局部数据集加载后飞到四川边界,截图看是否有环状墙。

**P1 CompositeTerrainProvider 骨架** → 验证:单测——availabilityState 在无 QM 区返回
Available(源=Ellipsoid),QM 区返回 Available(源=QM);`anyChildAvailable` 恒真。

**P2 内容路由 + 椭球瓦片渲染** → 验证:单测——纯空洞瓦片拿到椭球网格(h=0,gridSize²顶点);覆盖
瓦片拿到 QM;边缘子仍走 QM upsample。真机:未覆盖区从"粗台阶+巨墙"变"平滑椭球"。

**P3 椭球瓦片 skirt + 边界** → 验证:真机——椭球区无瓦片间裂缝;覆盖/未覆盖边界无人造巨墙(允许
诚实高度墙)。golden 全局数据集字节不变。

**P4(可选,若悬崖碍眼)边界 skirt 抬升** → 验证:边界缝闭合截图。

## 6. 风险 / 回归防护
- **全局数据集零回归**:椭球源永不命中,`git`/golden 字节级证明。
- **selector 不动**:纯内容层,selector 对拍不变。
- **性能**:未覆盖区现在会细化到 SSE(而非停粗叶子)→ 瓦片数增加。需给椭球区一个**较松的
  geometricError / 较浅 maxZoom**(椭球是平面,不需深细化)避免无谓细化。这是关键调参点。
- **upsample 交互**:确保椭球源瓦片不被误当 QM upsample 源(源标记隔离)。

## 7. 一句话
把 cesium "全局 availability" 的性质,用已存在的 `EllipsoidTerrainContentProvider` 给 partial 数据
集补齐:**未覆盖区永远可用为平滑椭球**,于是永不出现粗叶子巨墙;覆盖区 QM 不变;全局数据集下惰性
零改动。诚实的高度悬崖保留(非 bug),人造裙墙消除。

## 8. 落地状态 + 已知缺口(2026-07-08)

**已落地**(branch `feat/ellipsoid-fallback-composite-provider`):`content/CompositeTerrainProvider.{h,cpp}`
融合 QM + 椭球;`availabilityState` 地板;SDK opt-in `TerrainSourceConfig.ellipsoidFallback`(默认 OFF)。
- feat `55af54bb6` + fix `34b069010`。8/8 composite 单测 + 153/153 全套绿;flag OFF 结构性零回归(composite 不被构造)。
- 真机烟测:全局 ion 上开 fallback,40 瓦片 glError=0、地形正常不拍平不崩(zero-regression on-device)。

**⚠️已知缺口 P4-TODO(父终止空洞会拍平真实地形)**:当前 `isPureHoleQuad(key)` 只查 **4 个兄弟**是否全无 QM,
**未查父/祖先**。于是"覆盖父 P(有真实粗地形)+ 其 4 子全无 QM"(经典粗叶子)时,子瓦片被判 `Available(椭球 h=0)`,
而非 §4.2 设计意图的 **QM 上采样(裁 P 真实粗几何,保留高度)**。后果=覆盖区一级之下的未覆盖带被拍平成 0
而非续延真实粗高度。**边界情形(有兄弟可用)已被 fix `34b069010` 挡住**;此处是"父终止"情形的漏网。
- **影响很窄**:仅 partial 数据 + 未覆盖区**非海洋**(海洋处椭球≈海平面无差别)时可见。全局 ion 上不触发。
- **为何不现在修**:休眠路径(生产走全局 ion);且"椭球 vs 一级上采样"哪个视觉更好需 partial 数据真机看,盲修有风险。
- **正解方向**(将来接 partial 数据时):`isPureHoleQuad` 改为"祖先链是否全无 QM"判定(walk-up 早退),或在父是覆盖粗叶子时
  让子走 upsample 一级再椭球。自相似 fake provider 测不到此路径,需真实 partial QM 台架。
