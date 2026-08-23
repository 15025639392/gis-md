# MVT 矢量底图架构 — 机制与扩展参考

> **本文与 `docs/northstar/vector.md` 的分工**(别混):
> - **northstar** 回答「做到什么程度算好、现在到哪」——判据 `V*`、性能债 `P*`、已判死清单。
> - **本文** 回答「怎么搭的、要扩展该动哪」——数据流、分层、契约、扩展点。
>
> 判据与债不在本文重复,只引编号(如 V3 / P3)。本文写完即随架构变更更新,
> 不是冻结档案(那是 `docs/issues/*`)。

状态时点:2026-08-18(V26 样式系统三期收官 + §1b 驱动切面/七态 dump 后订正;
上一时点 2026-08-16 `e13de16f1`)。

---

## 0. 一句话

一条 MVT 数据源,**一个获取层**扇出到**三条表示各异的渲染路**:
面走栅格化进影像页合成、线走 SDF 距离场寄生地形 FS、点走 billboard 几何链。
分工的判据不是"数据类型",是**这类要素的视觉本质对应哪个物理量**,
以及该表示在 TBDR 上最便宜。

---

## 1. 全景:一块瓦片从 URL 到像素

```
                         MVT 瓦片 (http://…/{z}/{x}/{y}.pbf)
                                      │
                        ┌─────────────▼──────────────┐
                        │     MvtTileFetchCache      │  ← 唯一获取层(A.5 单一化)
                        │  L1 解码瓦 48 张 ~450KB/张 │     在途去重(inflight 合并)
                        │  L2 压缩字节 256 张 ~20KB  │     两层 LRU,L1 溢出降级 L2
                        │  失败不入缓存(重试自愈)   │     解码恰一次,内存恰一份
                        └─────────────┬──────────────┘
                                      │ shared_ptr<const MvtTile>
              ┌───────────────────────┼───────────────────────┐
              │                       │                       │
        【面】drape              【线】SDF 场            【点】符号几何
              │                       │                       │
   VectorDrapeImageryProvider   RoadFieldSource      MvtVectorSource
   ├ 冒充 ImageryProvider       ├ 经 PageStore        ├ VectorTileTree(选择/LRU)
   ├ VectorTileRasterizer       │   Config 注入        ├ worker 全链镶嵌(E1)
   │   → RGBA8 位图             ├ LineFieldRasterizer └ FeatureRenderLayer
   └ 失败回全透明图              │   → RGBA8 线段纹素     ├ 瓦片桶 tileBuckets_
      (不是 nullptr)            └ A==0 空哨兵           └ 整瓦原子替换
              │                    (失败安全)                  │
              ▼                       ▼                       ▼
      TerrainPageStore          TerrainPageStore         RenderCommand
      影像平面(与卫星同轨)      **场平面**(独立)        VectorPoint/Label
      512 层 RGBA8 128MB        64 层 RGBA8 16MB         billboard quad
      LRU + 间接纹理            独立 LRU + 独立 indir     锚点级遮挡判定
              │                       │                       │
              └───────────┬───────────┘                       │
                          ▼                                   ▼
                 地形 FS 内一次合成                     独立 draw(order 30/31)
                 eePageStoreCompose()                  深度测试**关**
                 (唯一治理点,六 shader 共用)          遮挡只看锚点
```

**关键结构事实**:
- 面与线**不产生独立 draw call**——都寄生在地形 pass 的片元着色器里合成(GPU ~0,见 V1/V2)。
- 点是唯一有独立 draw 的矢量内容(每桶 1 point + 1 label draw)。
- 场平面与影像平面**同页存储但不同生命周期**:场页有独立 zoom 封顶、独立 LRU、独立间接纹理(场专项步3)。

---

## 1b. 驱动视角:谁推数据流动(2026-08-18)

§1 画的是空间切面(数据往哪流)。本节是**驱动切面**(什么力量推它流、在哪会停)
—— 引擎是按需渲染(帧门控,idle 停帧),"帧"是稀缺资源;任何依赖帧循环推进
的收敛若不向帧门控申报,停帧即静默饿死,症状是"某东西不出现/停在半程"且零
报错。V27 家族一天连出五洞全是此病(排查全程见 northstar V27/V29 与
CLAUDE.md「帧收敛申报纪律」)。

```
外部驱动源:  [网络到达]        [worker 完成]        [渲染帧]           [用户输入]
                 │                  │                 │                  │
════════════════╪══════════════════╪═════════════════╪══════════════════╪═══════
                 ▼                  ▼                 ▼                  ▼
┌── 阶段(线程) ──────────┬─ 驱动 ─┬─ 节流/预算 ──────┬─ 收敛申报 ────────────────┐
│ ① fetch+decode(net/池) │ 网络   │ L1/L2 LRU 容量   │ Landing 票 mvtVectorLoad  │
│ ② 树选择(渲染线程)     │ 每帧   │ 无(全量重算)    │ (帧在即跑,无需申报)      │
│ ③ worker 镶嵌(decode池)│ ②派发  │ —                │ Landing 票(镶嵌在途)      │
│ ④ commit(渲染线程)     │ 每帧   │ 4 瓦/帧限速      │ Pumped 票 mvtVectorCommit │
│ ⑤ 字形烘焙(渲染线程)   │ 每帧重试│ 字形 ms 预算/帧  │ 谓词①(labelBakeSettled   │
│                          │        │                  │   区分在途/稳态)          │
│ ⑥ placement(渲染线程)  │ 每帧   │ 300ms 全量节流   │ 谓词② labelsAwaiting…    │
│ ⑦ fade(渲染线程)       │ 每帧dt │ 0.3s 收敛        │ 谓词③ hasPendingFades    │
│ ⑧ 贴地重钳(渲染线程)   │ 地形代次│ 2s wall-clock    │ 谓词④⑤ 队列/代次落后     │
│ ⑨ opacity 回写+出命令   │ 每帧   │ 逐 entry 免写    │ (⑥⑦的下游,同帧完成)     │
└──────────────────────────┴────────┴──────────────────┴───────────────────────────┘
  谓词①-⑤ = FeatureRenderLayer::hasPendingLabelWork(labelConverge Pumped 票 +
  Scene::hasConvergingWork ④ 回落项,两判据口径逐字一致防 audit 分歧)

横向失效轴(与纵向流正交):
  瓦片换代(z13↔z14) ──→ ④整瓦原子替换 ──→ crossTile id 继承(V29 窗+认领)
                                        └─→ 双代并存去重细代胜(V29 刀3)
  地形代次细化 ────────→ ⑧重钳(锚点高度重采,V27 家族第四缺口根修)
  换样式(V26/V28)────→ drape/场:epoch 原子换手;符号:Re-tess 全桶重镶
```

### 逐边可靠性账(2026-08-18 快照,变更时更新本表)

| 边 | 状态 | 依据 |
|---|---|---|
| ①→④ 瓦片流 | 干净 | Landing/Pumped 票齐(`MvtVectorSource.cpp` hasWorkTickets 区);异步回调自持数据(teardown race 家族已根修) |
| ④ 换代原子性 | 干净 | R*/V24 全有全无置换;V29 补 id 继承+双代去重 |
| ⑤-⑧ 符号收敛链 | 2026-08-18 起干净 | 五洞全在此段(原先零申报);终验 ShadowVerify「窗口干净」 |
| drape/场失效路 | 干净 | V28 epoch 原子换手,真机自证 |
| 帧门控申报机制 | **结构性弱点** | 申报是 opt-in 手写清单(五态谓词+三张票+一处 app 置脏);新环节漏报=静默饿死。兜底=ShadowVerify(demo 默认开)+ CLAUDE.md 纪律,非构造性保证 |
| 跨代状态真源 | 分散但已有聚合视图(2026-08-18) | 标注可见=7 个分散状态合成(驻留×烘焙×placement×fade×回写×锚点代次×遮挡)。**分布是设计本质非事故**(fade 按 crossTileID 键存正是为跨桶换代存活,强行合一存储会杀掉 V29 继承)——正解是单一**视图**:`FeatureRenderLayer::dumpLabelLifecycle` 七态只读 dump(demo `setprop debug.ee.labeldump` 免重编译触发),诊断从"逐层插探针"降为"dump 一眼看谁说谎" |

### 下一步结构投资(触发制,不预支)

优先级:**「队列即票」构造化**(队列/状态机构造时绑 ticket reconcile,消灭
手写枚举)。触发条件:再被"漏申报饿死"同族病咬一次,或符号管线新增异步
环节(如位图图标异步加载)时顺手做——凭单族样本抽原语大概率形状不对,
等第二个真实用例定形。
~~标注状态合一~~已重定性(2026-08-18):合一**存储**与 V29 跨代继承设计冲突
(疑似判死方向,再有人提先过这一关);合一**视图**已落地
(`dumpLabelLifecycle`,见上表)。

---

## 2. 核心决策:表示随负载,不是随数据类型

架构准则(用户认可):**矢量 = 数据模型;渲染表示是引擎内部自由。表示随负载,
判据 = 体验达标且最便宜。**

| 要素 | 视觉本质对应的物理量 | 选定表示 | 为什么不是别的 |
|---|---|---|---|
| 面(水/建筑块) | **归属 + 颜色**("这块地属于水") | 栅格化进影像页合成 | 距离场表达不了"归属";几何 stencil 实测 ~75ms GPU(发热真凶) |
| 线(路网) | **到中心线的距离** | SDF 距离场 + FS 解析解算 | 几何 ribbon 掠视块状;独立 overlay pass 实测 25-30ms;RTT 位图线宽会漂 |
| 点(POI/标注) | **屏幕上的注记**(恒定像素、可读性优先) | billboard + 锚点级遮挡 | 进场会随距离缩放到不可读;逐像素深度测试会把 quad 切一半 |

**这三条路互相不可替代,也不该强行统一**——这是本架构最重要的一条,
每次试图统一都被实测打回(见 northstar E 节「已判死」)。

### 演进脉络(为什么变成现在这样)

```
P1 几何上屏 → P3 贴地(方案A 高程采样) → P6a/d stencil 贴地(像素级)
    → P4 MVT 底图接入 → E1 瓦片即桶 + worker 全链(16107→216ms)
    → E2 分级过滤从数据侧搬回样式侧 → E4 drape 探索
    → 刀1 面→drape(75ms→0) → 刀2 线→SDF 场(stencil 链退役)
    → 场专项步1-3 + D2 线段纹素换代(线宽真像素恒定)
    → 符号五刀(刀0/A/B/C/D/E)+ A.5 获取层单一化
    → R*/V24 换代闪根修 → 符号遮挡按视觉语义重做
```

读法:**每一步都是"换表示"而不是"调参数"**,且每次换代都有实测数字背书。
这条脉络本身是架构决策的证据链。

### 2b. 表示选择矩阵(2026-08-23)

把 §2 的三行表扩成**可查的决策矩阵**:加新内容时先查「视觉本质 → 物理量 →
表示路 → 扩展点」,再决定复用哪条路或是否开第四条路。判据仍是一句:
**体验达标且最便宜**。

| 内容类 | 视觉本质(物理量) | 选定表示 | 上屏机制 | 实测成本 | 能力边界(能/不能) | 扩展点 | 失效成本类 |
|---|---|---|---|---|---|---|---|
| 面(水/建筑/landuse) | 归属+颜色 | drape 栅格化冒充影像 | 影像页合成 | GPU ~0(刀1 75ms→0) | 能:多色/分级/overzoom 现画;不能:逐要素交互高亮(像素级) | `VectorRasterStyle`+StyleDocument fill | Re-bake |
| 线(路网) | 到中心线距离 | D2 线段纹素 SDF 场 | 地形 FS 寄生 | GPU ~0(overlay 25-30ms→0) | 能:线宽像素恒定/多色(分类平面)/段内相位 dash;不能:沿折线弧长 dash(同语义=E 案)、逐要素渐变 | 分类平面/相位遮罩/通道 | Uniform/Re-bake |
| 点(POI) | 屏幕注记(恒定像素) | billboard 符号 | 独立 draw(order 30/31,深度关) | 每桶 1-2 draw | 能:分类图形/颜色/交互态高亮/锚点遮挡;不能:逐像素深度语义 | `TileSymbolCpu`/interaction | Re-tess/Uniform |
| 文本(标注) | 可读注记 | glyph quad+placement | 同 billboard | build p50 2.63→1.51ms | 能:CJK/避让/跨瓦 fade;不能:复杂 shaping/沿线文字 | GlyphAtlas/LabelPlacement | Re-tess |
| 轨迹(海拔着色/逐要素渐变) | 沿线连续物理量(海拔/里程/速度) | E 案几何线条带(方案 A ribbon,**非** SDF 场) | 独立 line draw | CPU 每顶点打包色 O(n)(镶嵌期);GPU 同 ribbon | 能:逐顶点渐变(海拔/里程/速度)、弧长 dash(同语义);不能:贴地像素级贴合(stencil 体积 mesh 无顶点色,需后置) | `a_color` 逐顶点 + `lengthSoFar` + `lineColorGradientByHeight` | Re-tess |
| 3D 挤出(建筑) | 体积/体量 | **未做(第四条路候选)** | glTF 类通路 | 未评估 | — | — | — |

**决策五问**(加新内容时):
1. 视觉本质是哪个物理量?(归属颜色 / 中心线距离 / 屏幕注记 / 体积 / 其它)
2. 该物理量在矩阵里已有表示吗?有 → 复用,扩展点照表动;
3. 没有 → 第四条路候选;先过 northstar E 节「已判死」清单再立项;
4. 立项时先量化性能预算(GPU/CPU/内存/上传),按「体验达标且最便宜」验收;
5. 四条路并存后才值得抽 #1 统一抽象(现在抽是过早)。

**已判死引用(防重复提议)**:stencil 贴地链(75ms GPU)、独立 overlay pass
(25-30ms)、标量距离场(漏画 63%)、向量距离场(中轴幽灵)、d+θ 紧凑编码
(误差回潮 0.048px)、E4 影像通路 drape(页纹素封顶近景糊)——详见 northstar E 节。

**对偶代价(每个便宜表示的账,立项时就该记)**:场无弧长 → dash 只能段内相位,
同语义要 E 案;drape 无逐要素交互(像素级),交互态只能在几何链做;billboard
无逐像素深度 → 遮挡按锚点判定。

---

## 3. 分层与职责

| 层 | 类 / 文件 | 职责 | 线程 | 不该做什么 |
|---|---|---|---|---|
| 获取 | `MvtTileFetchCache` | fetch + 解码 + 在途去重 + 两层 LRU | 任意(回调线程不定) | 不做选择(要哪些瓦是消费方的事) |
| 选择 | `VectorTileTree`(点)/ 页 determination(面/线) | 视口覆盖枚举、祖先回退、LRU | 渲染线程 | 不发网络请求(纯选择,由调用方回灌) |
| 过滤 | `SourceLayerRule` + `StyleFilter` | 层白名单 + 逐要素布尔过滤 | worker(不可变纯函数) | 不产生几何 |
| 转换 | `MvtFeatureConverter` | MVT → `Feature`(几何 + 属性) | worker | — |
| 镶嵌/栅格化/烘焙 | `FeatureRenderLayer::tessellateTileMesh` / `VectorTileRasterizer` / `LineFieldRasterizer` | CPU 重活 | **worker** | 不碰图集、不碰 GL、不碰地形采样器 |
| 上传/定型 | `commitTileMesh` / 页 upload | GPU 资源创建、整瓦原子替换、图集解析 | **渲染线程** | 不做 CPU 重活(闸的是上传成本) |
| 合成/解算 | `eePageStoreCompose`(`PageStoreSamplingGLSL.h`) | 影像 + 场一次合成,线宽像素解算 | GPU FS | 不内联到各 shader(唯一治理点) |
| 布局/避让 | `LabelPlacement` | 投影、碰撞、fade | 渲染线程,300ms 节流 | — |

---

## 4. 六条契约(扩展时最容易踩)

### 4.1 线程契约:worker 能做什么

worker **可以**:样式过滤求值、MVT→Feature、几何镶嵌、栅格化、场烘焙。
worker **不可以**:碰图集(GlyphAtlas/IconAtlas)、碰 GL、碰地形采样器。

推论(实际踩过):点符号的 quad 定型必须留在渲染线程(要图集),
所以 worker 只产 `TileSymbolCpu` **实例表**(经纬度锚点 + 已求值的样式),
`commitTileMesh` 才采地面高、查图集、展开 quad。
文字同构:commit 只存标签源,`bakeTileBucketLabels` 在字体就绪时补烘(幂等)。

> 为什么锚点存经纬度而不是 ECEF:贴地采样是渲染线程状态,worker 给 ECEF
> 等于把高度焊死在椭球面,山地下整批埋进地形。

### 4.2 zoom 三义(本仓最大的语义坑)

| 名称 | 含义 | 出现处 | 特性 |
|---|---|---|---|
| **瓦片 zoom** | 数据瓦自身的 z | `StyleFilter::zoomCompare`、`SourceLayerRule::min/maxZoom` | 固定 → **相机缩放不触发重镶** |
| **页 zoom** | 页存储按屏幕误差选的 z | drape/场的 `styleZoom`、场封顶 | 比地图直觉 zoom **高 ~2-3 档**(dpr + 屏幕误差) |
| **相机 zoom** | 由相机高度换算 | `StyleExpression::zoom()`、`VectorTileTree::zoomForCameraHeight` | 连续变化 |

三者都是裸 `int`/`double`,**类型上无法区分**,已经踩过两次:
场封顶设 14 导致 z≥15 的末梢路被样式 filter 整体滤掉(远山小径全灭);
宽度 ramp 停点按地图直觉设,落在可见区间外。

> **扩展提醒**:任何新的 zoom 参数,先问「这是三义中的哪一个」,并在注释里写死。

### 4.3 白名单 vs 逐要素过滤(踩过,代价是画面叠加 + 54.8ms GPU)

- `includeLayers` = **整层白名单**,空 = 全收。
- `layerRules` = 细则;**未列出的层是"全收不过滤",不是"跳过"**。

只给 `layerRules = {poi}` 不会排除 roads/water/building——它们会全量灌进几何链,
与场/drape 画出的内容**同屏叠加**。整层排除必须用 `includeLayers`。

### 4.4 获取归单一层,选择归消费方

三条消费路的**选择策略本就该各自独立**(drape/场要可见地形瓦的祖先;
符号树是视口覆盖枚举),但**获取必须共享**——否则同一数据瓦被拉三次、
解码三次(81ms/巨瓦)、存三份。

A.5 之前符号链自带 fetch+decode 栈且 `VectorTileTree` 按值再存一份,
是三重复。修法 = 注入共享 `MvtTileFetchCache` + `provideShared` 存 shared_ptr。

### 4.5 采样链单一治理点

`eePageStoreCompose` 是页存储采样 + 场解算的**唯一实现**,注入进六个片元
shader(gltf / terrain / terrainInstanced × GLES/Metal)。历史上两次在单条管线
漏配特性(合批漏拷场 uniform;位移路径喂错 UV),症状随管线选择时隐时现、
极难归因。`tools/check_pipeline_feature_contracts.py` 守卫四类:
单一治理点、特性搬运完备、相位打包口径、MSL 镜像结构同序。

### 4.6 渲染固定状态:面/线 与 符号**分道**

`validateMvpRenderCommands` 锁死固定状态,且两类断言不同:

| 命令 | depthTest | 依据 |
|---|---|---|
| `VectorFill` / `VectorLine` | **true** | 贴地几何,像素与 3D 位置一一对应,逐像素深度测试语义正确 |
| `VectorPoint` / `VectorLabel` | **false** | billboard 四角共用锚点深度;quad 像素**没有 3D 位置语义**,逐像素切割传达不存在的形状边界 |

符号的遮挡改由锚点判定(shader `eeSymbolTerrainVisibility`):
连续量(非布尔,避免临界点闪)+ 屏幕空间标定容差(非固定米数)。
三家引擎同解:maplibre 地形模式关深度测试、osgEarth 默认 `Depth(ALWAYS)`、
Cesium 默认 `depthTestAgainstTerrain=false`。

---

## 5. 扩展指南:要加 X,该动哪

### 加一个新 MVT **面**图层(如 landuse)
1. 数据侧确保该层在 mbtiles 里(`tools/mvt_demo/build_mbtiles.sh`)
2. `makeMvtDrapeStyle()` 加一个 `VectorRasterLayerPaint`(layer 名 + filter + fillColor + minZoom)
3. 完事——drape 通路自动栅格化进页合成,**无新 draw、无新上传路径**

### 加一个新 MVT **线**图层(如 railway)
1. `makeMvtRoadFieldStyle()` 的 grading filter 里放行该层
2. ⚠️ 场编码只有"距离",**没有按图层分色的能力**——所有线共用 FS 里的一个颜色
   uniform。要分色必须先扩通道(见 §7 不足-1)
3. 检查场封顶 `kMvtRoadFieldMaxZoom` 是否 ≥ 该层样式的最后一个分级档

### 加一个新 MVT **点**图层
1. `mvtOpts.includeLayers` 加层名(⚠️ 见 4.3),`layerRules` 加分级 filter
2. 数据侧带 `rank` 属性(准入截断依据,128 符号/瓦上限)
3. 样式:`pointImageExpr` / `pointColorExpr` 用 `match("kind", …)` 分类
4. 若要新图形:`SymbolShape.h` 加枚举 + `kSymbolSdfBody` 加解析 SDF 分支
   (GLSL/MSL **共用同一份文本**,故意破例不双份维护)

### 加一种新**表示**(如 3D 建筑挤出)
先回答:它的视觉本质是哪个物理量?再查 northstar E 节确认没被判死。
3D 建筑属"非贴地负载",**不该进三分工任何一条**——它是独立几何内容,
走 glTF 类通路(V6 明确"3D 挤出未做,定案为未来内容")。

### 加一个平台后端(Metal)
`eePageStoreCompose` 的 MSL 变体已在治理点里,但**从未真机验证**(V20 ❌),
且 instanced MSL 的精简 uniform 结构缺场参数、符号 shader 无遮挡判定。
守卫 D 类只检查结构体字段同序,**不检查特性齐全**。

---

## 6. 做得好的地方(带证据,不是自夸)

1. **表示随负载的分工**,每次换表示都有实测背书:面 75ms→~0、线 overlay
   25-30ms→~0、镶嵌峰值 16107→216ms。这是本架构最强的一条。
2. **失败安全是系统性默认**,不是零散补丁:场纹素 `A==0` 空哨兵、
   drape 失败回全透明图(不是 nullptr)、fetch 失败不入缓存、
   瓦片**整瓦原子替换**(宁可保留旧瓦也不留半张)、图标名不命中回落 circle。
3. **机器可查契约**多层并存,且真的抓到过 bug:采样链治理点守卫、
   MVP 固定状态校验(本次符号改状态就是它当场 abort 抓住的)、
   AI_INDEX 行号守卫、相位打包口径守卫。
4. **判死清单**(northstar E 节)——防止重复提议已否决方案,并写明死因。
   这个习惯在同类项目里罕见,直接节省重复试错。
5. **worker 化彻底且契约写在注释里**:重活无一在渲染线程,
   "必须渲染线程 / worker 可调"逐函数标注。
6. **量化文化,且允许处方被推翻**:P2 记录了"债务条目原写调容量即可,
   量完发现处方是错的";P3 记录了"原描述『内存涨』对图集是错的,
   真风险是满后永久丢字"。**承认判断错误比记录正确结论更有价值**。

### 做得好但有代价的

- 单一治理点让六 shader 自动同步,代价是 `eePageStoreCompose` 参数已达 **11 个**,
  所有特性挤进一个函数,继续加会失控。
- 三分工把每条路都优化到 ~0 GPU,代价是**三条路的代码几乎没有共享**(见下)。

---

## 7. 不足与结构性弱点(诚实)

> 这里只列**架构层面**的弱点;性能债与体验缺口见 northstar 的 `P*` / `V*`。

1. **三条消费路没有共同抽象层**。加一个新数据类型要从头决定走哪条路、
   从头接线;三条路的产物类型、生命周期管理、失效机制各写一套。
   现状能工作是因为只有三条且都稳定了,**再加第四条(如 3D 建筑)会暴露**。
2. **样式系统割裂 —— V26 三期收官后大部分已还(2026-08-18 订正)**。
   原文断言的两层后果均已失效:面 drape 补了
   `VectorDrapeImageryProvider::setStyle`(加锁快照,可与 requestTile 并发),
   场路补了 `RoadFieldSource::setStyle` + TerrainPageStore 失效原子换手
   (V28),统一入口 `Engine::setStyleTargets` + `applyStyleDocument`
   (StyleDocument A 案对象 JSON,fail-loud 契约 + Uniform/Re-bake/Re-tess
   三档成本类失效路由,掩码合成防"文档没写的字段被洗")。真机热改闭环:
   改设备 JSON→Skin→变色零重装;symbol 换肤 placement/fade 不重启
   (dump 自证)。设计:`docs/issues/vector-style-architecture-2026-08-18.md`。
   **残余**:`style/OverlayStyle.h` 平行类型系(旧 `VectorLayer` GeoJSON
   编辑演示路)仍在 demo 上线且未并入 StyleDocument —— 判缓,唯一消费者
   是编辑演示层,正路是日后退役合并进 FeatureRenderLayer(归用户拍板)。
3. **线的样式表达力被编码格式卡死**:D2 场编码只有"距离 + 方向 + 端点余量",
   **没有图层/分类通道**,而且即便有位、颜色也在 uniform 侧(全局一个
   `u_roadFieldColor`,FS 里 `mix(base.rgb, roadFieldColor.rgb, roadCov)`),
   所以**全图线只能一个颜色**。多色路网必须扩通道或加平面 —— 这是
   "表示选得便宜"的对偶代价,当初该记进立项。
   **dash(V9)要分清两种形态**(推断,未实测):
   - `FeatureRenderStyle` 的同语义 dash(`lineDashPeriodMeters`,**沿折线
     累积弧长**)编码**不支持** —— fwd/back 上限 `kLineFieldClampMaxTexels
     = 1.5` texel、4bit 量化,拿不到全局弧长;
   - 不扩通道能做的是**段内相位** dash(`phase = dot(pagePos, dir) mod
     period`),代价是每个拐角相位断裂、周期是页空间量而非路上米数。

   即走场只能拿到降级形态,要同语义 dash 得走 E(几何线条带)——
   与 northstar 把 E 立为战略备选一致。
4. **zoom 三义无类型区分**(§4.2),已踩两次。裸 `int` 传递,靠注释约束。
5. ~~**样式硬编码**~~ **已收官(2026-08-18)**:
   面/线/符号样式走 StyleDocument 设备侧 JSON(见 #2),分级规则在
   `MinimalGlobeDemoConfig.cpp` 工厂 + `test_mvt_basemap_grading` 守卫;
   数据源 URL 走设备侧 `sources.json`(与 style-*.json 同目录约定,
   `parseDemoSourceOverrides` fail-loud:未知键/非字符串整份拒收回落内置)
   —— 换城市/换源改 JSON 重启即可,真机 A/B 自证(外置坏端口→矢量全缺席
   `xt=0`,删配置→内置恢复 `xt=468`)。
   **刻意的边界**:只做**启动期外置**(重启生效);运行期热切源不做——
   涉及 provider 重建 + 缓存失效 + V28 换手扩面,是独立专项的体量。
   zoom/tileSize 等源参数也不外置(外置源必须与编译期分支同构,fail-loud
   拦截越界键)。
6. **符号链是"复活的退役链",命名与职责脱节**:`kEnableMvtBasemap` 现在的
   真实职责是"点符号通路总开关",但名字、注释、`includeLayers`/`layerRules`
   的语义坑都是历史沉积。**新人按名字理解一定会错**。
7. **Metal 侧系统性滞后且无守卫**(V20 ❌):场解算从未真机验证、
   instanced MSL 缺场参数、符号无遮挡判定。守卫只查结构体同序,不查特性齐全。
8. **测试覆盖偏"数据/算法",渲染状态与 shader 缺执行级守卫**。
   本次符号遮挡改动 host **188/188 全绿却真机首帧 abort**——
   `validateMvpRenderCommands` 的 `VectorPoint/Label` 分支从无主机测试构造过。
   已补两条,但同类空洞可能还有;shader 在 host 无执行级守卫另见地形 T-P6。

### 若要还这些债,建议顺序

| 优先级 | 债 | 理由 |
|---|---|---|
| ~~高~~ ✅ | #2 样式统一 + #5 样式外置(**判据 = northstar `V26`**) | **2026-08-18 三期收官**(StyleDocument + 双路 setStyle + 失效路由,真机热改闭环);同日收尾:symbol 换肤像素补验(placement 跨换肤存活)+ 数据源 URL 启动期外置(见 #5)。唯一残余:OverlayStyle 交互路退役合并(判缓,归用户拍板) |
| 中 | #3 线分类通道 | 直接卡住**多色路网**;dash(V9)只被卡住"同语义"那一半(见 #3)。~~需与 P7 容量债一起算~~ —— P7 已于 2026-08-15 量化后**结清判定不修**,不再是前置 |
| 中 | #6 命名/开关正名 | 纯清理,便宜,防止新人踩坑 |
| 低 | #1 统一抽象 | 现在抽象是过早;等第四条路真出现时再抽 |
| 低 | #7 Metal | 无设备,阻塞在硬件 |

---

## 8. 关键文件索引

| 关注点 | 文件 |
|---|---|
| 获取层 | `data/MvtTileFetchCache.h/.cpp` |
| 解码/转换 | `data/MvtDecoder.*`、`data/MvtFeatureConverter.*` |
| 面 | `providers/VectorDrapeImageryProvider.*`、`data/VectorTileRasterizer.*` |
| 线 | `providers/RoadFieldSource.*`、`data/LineFieldRasterizer.*` |
| 点/符号 | `data/MvtVectorSource.*`、`data/VectorTileTree.*`、`layers/FeatureRenderLayer.*` |
| 标注避让 | `layers/LabelPlacement.*`、`renderer/GlyphAtlas.*`、`renderer/IconAtlas.*` |
| 样式/过滤 | `data/VectorRasterStyle.h`、`data/StyleFilter.h`、`data/StyleExpression.h` |
| GPU 合成 | `renderer/PageStoreSamplingGLSL.h`(唯一治理点)、`renderer/TerrainPageStore.*` |
| 渲染状态契约 | `renderer/RenderCommand.cpp`(`validateMvpRenderCommands`) |
| 守卫 | `tools/check_pipeline_feature_contracts.py`、`tools/line_field_capacity_sim.py` |
| demo 接线 | `examples/android/MinimalGlobe/MinimalGlobeDemoConfig.h/.cpp`、`GLESView.cpp` |
| 数据准备 | `tools/mvt_demo/`(提取 / 切瓦 / 起服务) |

**相关文档**:
- `docs/northstar/vector.md` — 体验判据 `V*` / 性能债 `P*` / 已判死清单
- `docs/northstar/imagery.md`、`docs/northstar/terrain.md` — 页存储与地形侧判据
- `docs/issues/vector-data-system-design-2026-07-07.md` — 最初的总设计(P0-P6 分期,已大部分被换代取代,读时注意时点)
