# 高德矢量瓦片链路北极星

> **状态快照：2026-08-27，基线提交 `679845a7`。**
>
> 本文是高德专属的长期维护北极星：回答「高德链路做到什么程度算正确、哪里有证据、哪里仍有债、改动后怎么验收」。
> 通用矢量体验判据仍以 [`docs/northstar/vector.md`](vector.md) 为上位规范；MVT 的通用机制、扩展边界和表示选择见 [`docs/mvt-vector-architecture.md`](../mvt-vector-architecture.md)。
> `docs/issues/*` 只记录一次性事故和修复过程，不替代本文。

## 使用协议

- `A-V*` 是高德视觉/正确性判据，`A-L*` 是生命周期与异步契约，`A-P*` 是性能与容量，`A-O*` 是可观测性与排障。编号只增不改，状态改变必须附证据。
- 状态含义：✅ 已达成（有测试、日志或真机证据）；⚠️ 部分达成/有已知缺口；❌ 未做；🔒 目标或阈值待拍板。
- 【机制】由测试、计数、日志和帧时自证；【观感】需要固定场景的真机画面，不能用“代码看起来对”替代。
- 性能优化必须减少重复请求、重复解码、无效遍历、阻塞、冗余上传或无效旧视野工作；不得以缩小全球覆盖、降低必要 LOD、隐藏可见图层或牺牲交互稳定性换取数字。
- 任何新增高德图层、请求类型、坐标转换、样式层级或 worker 队列，必须先更新本文对应判据、故障判别和测试锚点。

## 北极星一句话

> **任何全球、近景、连续缩放或快速平移视野，都应保持正确的地理覆盖和层级关系；高德数据从版本探测到 GPU 提交必须可追踪、可收敛、无重复工作；细瓦未完整就绪时旧覆盖继续顶住，异步迟到结果不能制造空洞、重影、错位或假收敛。**

这句话包含四个不可拆开的目标：

1. **位置正确**：瓦片矩形、经纬度、GCJ-02 转换和 raw 网格方向一致，点不进江、线不漂移、面不跨瓦错位。
2. **拓扑正确**：孔洞、凹面、disjoint rings、边界裁剪和复杂多边形不会破碎或填反。
3. **层级正确**：水系/植被/地块/建筑/道路/POI 的职责和压盖顺序稳定，细化与回退不出现空洞或父子连续几何重叠。
4. **链路可控**：网络槽位、CPU decode、tessellation 和渲染线程 commit 分开治理；相同 key 不重复拉取/解码；视野变化最终归零且不会把旧工作伪装成已取消。

## 范围与非目标

### 本文范围

- 高德 `web/init` → `web_map/get_tile` → 签名 `tile_url` → 容器解压/解码 → Feature → tessellation → render-thread commit 的完整闭环。
- `regions`、`water12`、`main`、`POI` 四个高德消费源，及它们共享/隔离的 cache、worker pool 和样式层级。
- 高德 4326 等距圆柱瓦片、离散数据 zoom、raw Y 方向、坐标转换、环拓扑和瓦片边界处理。
- 全球高空、地平线/斜视、近景、连续缩放、快速平移、网络失败和样式/视野换代。

### 非目标

- 通用 MVT/FeatureStore 产品体验（看 `docs/northstar/vector.md`）。
- 高德服务端协议的长期兼容承诺；manifest/POI schema 仍需随服务端版本样本回归。
- MVT 道路线场（`RoadFieldSource` → D2/`TerrainPageStore`）的新功能扩展。该路径已标记“即将废弃”，只保留兼容和回归用途；新高德线功能走 `FeatureRenderLayer` 几何路径或另立表示方案。
- 将本地仓库中“Cesium 对齐候选”写成已经逐行核实的上游结论。当前约定的 `/Users/ldy/Desktop/work/cesium-native` 不存在，相关内容必须标为待上游复核。

## 当前判据水位

| 编号 | 判据 | 类型 | 状态 | 当前证据/差距 |
|---|---|---|---|---|
| **A-V1** | 全球高空视野使用完整粗档覆盖，不退化为中心孤岛 | 机制+观感 | ⚠️ | 机制✅：高空 canonical zoom 映射到 z3，Amap 4326 在 z3 为 `8×8=64` 瓦；`AmapGlobalLodUsesCoarseTiersInsteadOfCenterIsland`。PHK110 观感曾验，但原始日志/截图未作为仓库资产保存，需固定场景复验后转全绿。 |
| **A-V2** | 相邻瓦片在经纬度、Y 方向和 GCJ 转换后无横向/纵向错位 | 机制+观感 | ⚠️ | raw Y 翻转、坐标缩放、GCJ 及 manifest 精确选 URL 已有测试；全球反经线/极区和真实 amap.com 逐视角回归尚未自动化。 |
| **A-V3** | 多边形孔洞、凹面、独立环和越界裁剪不破碎、不填反 | 机制 | ✅ | `EvenOddWinding*`、`Concave*`、`DisjointRings*`、`CompoundMask*`、真实样本三角形边界测试。 |
| **A-V4** | 水系、植被、地块、建筑、道路和 POI 压盖顺序正确 | 机制+观感 | ⚠️ | `water12` 独占近景 30001，`main` 过滤 30001，水/绿地与 main 地块、建筑、道路的交错 paint order 已固定；真实服务端新类别/新版本层级仍需回归。 |
| **A-V5** | 点/线/面保持同一地理位置，不出现“工业园在江里/篮球场在江里” | 机制+观感 | ⚠️ | 统一 Amap scheme、raw Y、scale、GCJ 路径已锁；需要持续用 POI/道路/水系交叉样本做空间关系回归。 |
| **A-V6** | 细化与回退无空洞；连续几何不父子同框重叠 | 机制 | ✅ | `GeometryReplace`、R* 原子换手、`LodSwapIsAtomicNoHoleNoOverlap`、`GeometryReplaceRenderSetHasNoAncestorPairs`。 |
| **A-V7** | 单个瓦片失败不毒化无关层，旧覆盖留在屏上，重试可收敛 | 机制 | ✅ | 失败回调、指数退避、旧 active 保活、`RetryableCommitFailureDoesNotActivateAndRetriesMesh`、`TemporaryFailureRetriesAndRecovers`。 |
| **A-V8** | 视野稳定后异步工作最终归零；按需渲染不会因未申报工作停在半成品 | 机制 | ⚠️ | Landing/Pumped 票、inbox 排空和 `suspend()` 已有守卫；跨四 source 的统一收敛断言仍缺。 |
| **A-L1** | `request/pending → decoded/failed → ready/active` 生命周期闭环 | 机制 | ⚠️ | HTTP/manifest/解码返回失败、tess worker 异常和 retryable commit 已有终态/重试测试；任意 DecodeTraits 抛异常的通用保护尚未建立，不能写成所有异常已闭环。 |
| **A-L2** | 网络在途/总 pending、CPU worker、渲染 commit 是三个独立预算 | 机制 | ✅ | Curl 20 槽（High 预留 2）、Tree 每 source pending 上限 64、decode/POI/tess 分池、source tess 上限 8、commit 每 source/update 默认 4。具体数值是本地策略，不冒充 Cesium 规格。 |
| **A-L3** | 可见/中心/紧迫工作优先，单 source 不长期饿死其他 source | 机制 | ⚠️ | Tree 中心优先、type1 与 POI 解码分池；共享 Curl 仍按 Low 竞争，尚无跨 source 公平性 p95 gate。 |
| **A-L4** | 父/祖先覆盖在 replacement 子树完整前继续顶住，交接同帧完成 | 机制 | ✅ | R* 置换单元要求全部 ready，再 commit 新瓦并 drop 占位者；失败保留旧占位者。 |
| **A-L5** | 渲染线程只做有界 finalize/upload，且预算覆盖全部高德 source | 机制 | ⚠️ | 当前 `maxTileCommitsPerUpdate=4` 是每个 source 独立计数；R* 单元不可拆时可超额，尚无全链 `commitMs` gate。 |
| **A-L6** | 取消解除真实成本并保证 exactly-once；不能制造 `pending=0` 假收敛 | 机制 | ⚠️ | 当前只有逻辑取消：旧 key 从 pending/视野中移除，迟到 mesh 由 `viewEpoch` 丢弃；HTTP、已排队 decode/tess 尚不能真正 cancel。 |
| **A-P1** | 同一 inflight/缓存驻留期内，相同数据 key 网络只取一次，跨 regions/main/water12 共享 type1 raw 与 decoded payload | 机制 | ✅ | 三个 source 共用同一个 `gAmapRegionCache` typed cache，在途合并、L1 decoded 和 L2 raw 均可复用；raw L2 淘汰后允许重新联网。 |
| **A-P2** | 解码不被几何镶嵌的 FIFO 队头阻塞，POI 独立前进 | 机制+性能 | ✅ | type1 decode、POI decode、tess 分池；`BlockedTessellationDoesNotStarveDecode`。 |
| **A-P3** | 设备自适应只约束后台并发，不削减全球覆盖或必要细节 | 机制 | ✅ | 预算只按核心数/内存计算；无 `PHK110/V1818T` 型号特判。按当前函数，8 核且 >12GiB 自动推导 type1 decode=2、POI decode=1、tess=3；启动属性可覆盖，真机必须以 `MvtWorkers` 日志为准。 |
| **A-P4** | 高空全球收敛成本由可测阶段组成，不把网络等待误判成渲染瓶颈 | 性能 | ⚠️ | 现有聚合日志覆盖 cache、decode pool、tess pool、tree、commit；尚无 version/manifest/signed GET 三段耗时。基线提交 `679845a7` 前后的 PHK110 固定全球 64 瓦对照约为 68s→1.75s，但原始日志未入仓，复测前不能当稳定 SLA。 |
| **A-P5** | 全高德 source 合计的主线程 finalize 有统一上限 | 性能 | ⚠️ | 当前只有单 source 项数预算；目标为记录总 `commitMs` p50/p95/max 和项数，在 60fps 下把 p95 控制在约 4.2ms 内、max 不超过半帧（阈值需真机基线确认）。 |
| **A-P6** | 缓存容量有界；raw L2 保留窗口内回摇不重复网络 | 性能 | ✅ | typed decoded L1=48、raw L2=256；`EvictedTileRefetchesFromRawTierNotNetwork`、跨消费者 raw 复用测试。raw L2 也淘汰后会重新联网。 |
| **A-O1** | 异常可定位到请求、解码、镶嵌、提交或层级阶段 | 运维 | ⚠️ | `AmapType1Cache`、`MvtPool`、`AmapSource`、`VectorTessSlow` 提供聚合/慢任务信号；fetch/decode/commit 失败尚缺按 key/stage 的统一结构化日志。 |

## 链路总图：一块瓦片如何到达像素

```text
相机/ECEF
  └─ horizonViewRectangle + cameraHeight
      └─ amapViewZoom（连续相机 zoom）
          ├─ regions     ≤ 11.5：z3/6/8/10 粗区域
          ├─ water12     > 11.5：固定 z12 水/绿地底板
          ├─ main        z3/6/8/10/12/14：道路/建筑/轨道/地块
          └─ POI         z3/6/8/10/12/14：type-0 点标签
              └─ VectorTileTree
                  ├─ Amap scheme tileRange（跨反经线拆段）
                  ├─ 离散 data zoom 选择 + maxTilesPerView 降档
                  ├─ desired/render/request 集合、祖先回退
                  └─ typed MvtTileFetchCache.request(key)
                      └─ amapFetchTile(key, requestType)
                          ├─ ①共享 GET /web/init?key= → version stamp
                          ├─ ②POST /web_map/get_tile → manifest tile_urls
                          │    精确匹配 group + x_y_z，禁止取数组第一项
                          └─ ③GET 签名 tile_url → 4-byte length + gzip(protobuf)
                              └─ L2 raw cache / inflight 合并
                                  └─ decode pool
                                      ├─ AmapDecodedTile（type1/type2 parts）
                                      └─ typed decoded L1
                                          └─ decoded inbox → tree.provideShared
                                              └─ tess pool
                                                  ├─ Amap part → Feature
                                                  ├─ Y/scale/GCJ/环拓扑/裁剪
                                                  └─ FeatureRenderLayer::tessellateTileMesh
                                                      └─ mesh inbox
                                                          └─ render thread ingest
                                                              ├─ rulesEpoch/viewEpoch/taskId 闸
                                                              ├─ R* 原子 replacement
                                                              └─ commitTileMesh → GPU bucket/RenderCommand
```

### 每一阶段的线程边界

| 阶段 | 所在线程 | 可以做 | 禁止做 |
|---|---|---|---|
| 相机/Tree 选择 | 渲染线程 | 视口矩形、zoom 吸附、父子覆盖、请求排序 | 直接网络、解码、三角化 |
| HTTP | Curl scheduler | version、manifest、签名 GET、失败回调 | 改 tree、改 GL、在回调线程销毁 `HttpRequest` |
| 解码 | type1/POI decode pool | inflate、protobuf/Nebula 字段解析、轻量 typed payload | 访问 GL、改 active/ready 集 |
| Feature/tessellation | tess pool | 过滤、坐标转换、环归一化、CDT/clip、mesh CPU 产物 | 图集、地形采样器、GL/GPU 资源 |
| inbox/commit | 渲染线程 | 丢弃过期结果、GPU upload、active/drop、原子换手 | 在主线程做大规模 protobuf/几何计算 |

worker 结果必须先入 inbox，再释放 Landing 票；否则按需渲染可能已经被唤醒却看不到产物。`suspend()` 必须排空 inbox、推进 epoch、释放 active/ready，并同步 Pumped 票。

## 四个高德 source 的职责和唯一数据归属

| source | 请求/载荷 | 选择范围 | 过滤/表示 | 层级策略 |
|---|---|---|---|---|
| `amap-regions` | request type 1；完整 `AmapDecodedTile` | z3/6/8/10，视图 zoom ≤11.5 | 只取 decoded part `type=2` 区域；远景粗水/绿地/地块 | 远景连续粗底，GeometryReplace，paint order 约 10–30 |
| `amap-water12` | 与 regions/main 共用 type1 raw/decoded | 固定 z12，视图 zoom >11.5 | 只取 `classCode=30001` 水/绿地 | 近景水系接缝底板；绿地约 20、水约 50，处于地块 30 与建筑/道路 60+ 的交错顺序中，不是全球常显层 |
| `amap-main` | request type 1；完整 type1 payload | z3/6/8/10/12/14 | type1 线、type3 建筑、type4 轨道；z12+ type2 只保留非 30001（例如 30002 地块） | 道路/建筑/细地块；30001/30003 透明，避免水系重复和错误压盖 |
| `amap-poi` | request type 2；POI 专用解码 | z3/6/8/10/12/14 | 只取 decoded part `type=0` 点标签；独立 `gAmapPoiDecodePool` | billboard + label，paint order 100，点位保留经纬度锚定 |

### 两个“type”不能混淆

| 名称 | 含义 | 示例 |
|---|---|---|
| **请求 type** | manifest 组：1=`building_region_road_transit`，2=`poi_region_road_transit` | `amapFetchTile(key, 1/2)` |
| **decoded part.type** | 容器内部几何类型：0=POI、1=线、2=区域、3=建筑、4=轨道/区域混合 | `AmapRegionsToFeatures` 只收 part 2；`AmapPoiToFeatures` 只收 part 0 |
| **classCode/kind** | 样式、过滤和压盖语义，不是几何类型 | 30001 水/绿地、30002 地块、90001/200xx 建筑/道路类别 |

任何将 `classCode` 当作几何类型、把 decoded part 2 当成线、或把 request type 2 的 POI 容器走通用线面解析，都是“篮球场在江里/线面错位/多边形破碎”的高风险改动。

## 数据协议、zoom 和坐标不变量

### manifest 与 URL

1. `web/init` 的 `tile.v` 是全局数据版本；成功前不缓存版本，失败后允许重新探测。
2. `web_map/get_tile` 是 POST 表单，包含 `version`、`pbf_version=v2`、`access_oversea=1`、`data_source=1`、`multi_lang=0` 和 JSON `tiles`。
3. `contain_range=2` 可能返回邻近瓦片和多个 group；必须同时匹配 `group` 与 `id=<x>_<y>_<z>`，不能把第一个 URL 绑定给当前 key。
4. signed URL 响应是“4 字节大端长度 + gzip(protobuf)”容器。manifest 无 `tile_urls` 可能表示无数据，但当前异步链仍会将“无匹配 URL”收敛为空失败回调；后续日志需要区分合法无数据与协议/传输失败。
5. key 只允许进入配置/运行时请求，不得写入本文、日志样例、提交信息或测试输出。

### 三种 zoom 必须分开书写

| 名称 | 含义 | 高德规则 |
|---|---|---|
| 相机/canonical zoom | 由相机高度连续换算的视野尺度 | `amapViewZoom = clamp(log2(4e7/cameraHeight), 0, 24)` |
| 数据 zoom | 服务端实际可请求的档位 | `canonicalZ + 1` 后吸附到 3/6/8/10/12/14 |
| 样式 zoom | Feature/StyleFilter 的数据语义 | 由 `part.z`/规则判断，不能拿相机 zoom 直接替代 |

高德数据档位实现以 [`AmapTileManifest.cpp`](../../scaffold/src/earth_engine/data/AmapTileManifest.cpp) 的 `amapDataZoom()` 为准；注释中的旧区间文字不具有优先级。Tree 选择先映射再按 `supportedZooms` 向下吸附，工作集超 `maxTilesPerView` 时先整体降档，不能按枚举顺序截掉全球视野的一半。

### Amap 4326 tile scheme

- CRS：EPSG:4326，等距圆柱，2:1；level 0 为 `1×1` 世界瓦片，z 为 `2^z × 2^z`。
- x：经度从 -180° 向东；y：`0` 在北纬 90°，向南递增。
- `tileToRectangle`：`west=x/n*360-180`，`east=(x+1)/n*360-180`，`north=90-y/n*180`，`south=90-(y+1)/n*180`。
- `viewRect.west > east` 时先拆反经线两段；不得把它当成一个跨全球的大矩形。
- source、tree、worker tessellation context 必须使用同一个 `TileScheme::createAmapGeographic()`；不能让 fetch 用 Amap key、render 用 XYZ key。

### raw 网格到经纬度

1. Nebula raw 几何 Y 是 bottom-up；统一先转 canonical top-down：`canonicalY = 4096 - rawY * scale`。
2. 再用 `amapTileLocalToLngLat(x, canonicalY)` 转入瓦片经纬度；不能对已经翻转的 ring 再翻一次。
3. `scale` 按 layer type/data z/kind 选择：

| 几何 | 规则 |
|---|---|
| type 0 POI | z3=8；其余通常 4 |
| type 1 线 / type 4 轨道 | z3=8；z6–12=4；z14+=2 |
| type 2 区域 | 默认 4；kind 60/64/80 走 line-grid 规则 |
| type 3 建筑 | 1/16 |
| class 20017 detail | 0.5（按实现的专用细节网格） |

4. 坐标转为 WGS84 的 GCJ-02 反偏移只作用于经纬度；不得改 tile-local、tile key 或瓦片矩形。

## 面、线、点的几何正确性契约

### 面：环、孔洞和裁剪

- type2/type4 区域环按 even-odd 语义处理，不能假设供应商顺/逆时针可靠。
- 先用 interior point/containment 求嵌套深度：偶数深度为外环、奇数深度为孔洞；独立负环要恢复为独立外环。
- canonical top-down 坐标必须同时用于环归一化、CDT/clip 和最终经纬度转换，不能在其中一个阶段切回 bottom-up。
- canonical 缓冲窗口为 `x∈[-256,8448]`、`y∈[-256,4352]`。环没有真正越出窗口时跳过重复 CDT；只有越界时才使用 triangulate-then-clip。
- CDT 后每个三角形仍需通过 even-odd mask；所有三角形质心和顶点必须落在源多边形允许区域，孔洞内不得生成填充。
- 渲染层可能再次做 Feature tessellation；两次三角化必须保持坐标系和 mask 语义一致，不能把“已裁剪三角形”再按错误 Y 方向解释。

### 线与点

- type1/type4 线每个 ring 是一条 LineString；不能把区域边界 ring 当作可填充面，也不能把线的 raw grid 当成 POI grid。
- type0 POI 每个 label 一个点；锚点保存经纬度，地形采样/quad 展开留给渲染线程。
- `VectorFill`/`VectorLine` 使用深度测试；`VectorPoint`/`VectorLabel` 使用锚点级遮挡，不用逐像素深度切 billboard。
- 点/线明显落入水体时，先比较它们与同一 tile 的 water/region 面的经纬度和 GCJ 转换结果，再看样式；不能先通过隐藏 POI 或降低 LOD 掩盖问题。

## 层级与压盖北极星

高德服务的“同时返回多档数据”不是让四个 source 无条件叠画。我们的职责是给每类几何一个唯一的可见责任：

| 内容 | 唯一责任源 | 规则 |
|---|---|---|
| 远景水/绿地/连续区域 | `regions` | z3/6/8/10，视图 zoom ≤11.5 |
| 近景水/绿地接缝底板 | `water12` | 固定 z12，视图 zoom >11.5；绿地约 20、水约 50，地块 30、建筑/道路 60+ |
| 近景地块 | `main` | z12+ 保留 30002；30001 透明 |
| 道路/建筑/轨道 | `main` | 按 geometry type 先判，再按 class/kind 排序 |
| POI/文字 | `POI` | 独立点/标注 draw，paint order 100 |

硬规则：

- 水系 30001 不能由 `main` 的 z14 细面再次上色；这会把服务端 z14 局部空档放大为平行双带和瓦片横向错位。
- `water12` 只近景启用；全球固定 z12 + 256 瓦会重新形成“重庆中心水面岛”。
- `regions` 近景 suspend；否则 z10 粗面会盖住 z12/z14 细面，产生粗像素块和破碎边缘。
- 未知 class 默认透明或明确记录，不得默认当作陆地色覆盖水系。type3 建筑的 geometry type 优先于共享 classCode。
- `GeometryReplace` 的 render set 不允许 active 同时出现 ancestor/descendant 连续几何；R* 只允许旧占位者暂时留场，不允许新旧连续面同框。

## 生命周期、取消和原子换手

### 正常闭环

```text
requestTiles → pending
  → fetch/inflight
  → decoded inbox → provideShared
  → tessellating
  → mesh inbox → ready
  → render-thread commit
  → active
```

- 每条 fetch 成功、HTTP 失败、manifest 不匹配、gzip/protobuf 失败都必须回调；漏回调会让 cache inflight 和 tree pending 永久不消化。
- worker 异常必须清掉 tessellating，mesh 失败不能污染 decoded tile；当前 key 仍可见时下一次 update 应能重试。
- 结果携带 `rulesEpoch`、`viewEpoch`、`taskId`；样式换代、快速平移、source suspend 后迟到结果只能被丢弃，不能 commit。
- R* replacement unit 内，旧占位者在所有替换瓦 ready 前继续 active；单元完整后同一 update commit 新瓦并 drop 旧瓦。单元内某次 GPU commit 可重试失败时，已提交的新瓦必须回滚，避免父子同框。

### 当前取消语义（诚实标记）

当前 `FetchFn` 形态是 `void(key, callback)`，没有 CancellationToken/HTTP handle 贯穿 cache、decode 和 tessellation。视野变化会：

- 从 Tree 的 desired/pending 集移除旧 key；
- 用 `viewEpoch` 丢弃旧 mesh；
- 让已发出的 HTTP、已排队 decode/tess 自然完成。

因此 `pending=0` 只能说明“Tree 不再等待旧 key”，不能说明真实 CPU/网络成本已经停止。未来 A-L6 的完成定义是：

1. cancel token 能传到 HTTP、decode、tess 三段；
2. 取消释放真实槽位，并保证 callback exactly once；
3. tile 仍存在但请求被 stale cancel 时进入可观察终态/退避，不能永久停在 `ContentLoading`；
4. destroy 先 cancel，再等待 callbacks drained，之后才销毁共享状态。

## 性能预算与测量口径

### 当前硬预算

| 资源 | 当前策略 | 不能误读为 |
|---|---|---|
| Curl | 全局最多 20 个在途；High 预留 2，矢量 Low 最多占 18 | 设备专属高德限制 |
| type1 decode | 设备分档；8 核且 >12GiB 自动推导 2 | “全球只解码 2 瓦” |
| POI decode | 独立池，当前自动预算固定为 1 | 与 type1 共用 FIFO |
| tessellation | 设备分档；8 核且 >12GiB 自动推导 3，source 各自最多 8 个在途 | 可以无限并行或绕过主线程预算 |
| Tree 工作集 | 高德 source `maxTilesPerView=256`，默认 `maxPendingRequests=64` | 可以把视野截成中心岛；高空先降数据档 |
| cache | decoded L1=48，raw L2=256 | L1 淘汰等于重新联网 |
| commit | 每 source 每次 update 默认最多 4 项；R* 完整单元不可拆 | 全高德合计最多 4 项 |

### 必采指标

每 120 帧输出一次：

```text
AmapType1Cache fetch/refetch/hit/rawHit/resident/rawBytes
MvtPool type1Decode/poiDecode/tess: threads, queued, active, done,
        queueAvg/queueMax, workAvg/workMax
AmapSource regions/water12/main/poi:
        z, desired, scanned, render, request, pending, tess, ready,
        active, ancestorPairs, treeMs, commitMs
```

慢任务输出 `VectorTessSlow`，至少包含 source、z/x/y、feature 数、convertMs、tessMs。性能定位必须按顺序区分：

1. 版本/manifest/签名 GET 的网络两跳延迟；
2. raw cache 命中、inflight 合并和字节复制；
3. inflate/protobuf/POI 专用解析；
4. Feature 深复制、坐标转换、CDT/clip、渲染层二次 tessellation；
5. tess queue wait/work；
6. render-thread commit/GPU buffer upload；
7. camera/tree 遍历和高空视口枚举。

### 性能验收门槛

- 固定全球相机、固定网络/缓存条件，记录“首个可见瓦”“本视野所有启用 source 稳定”“pending/tess/ready/work 归零”三个里程碑；被 gating suspend 的 source 应保持零状态，不能只报一个总耗时。
- 固定近景和快速平移各跑一次，要求旧视野 mesh 丢弃计数可解释，新视野不被旧任务饿死。
- 全链 commit 必须增加统一聚合统计；目标是 60fps 场景总 `commitMs` p95 约不超过 4.2ms、单帧不超过半帧，R* 不能以“不可拆”为理由无限突破。阈值在 PHK110 真机基线完成前标为目标，不得写成已达成。
- 任何性能 A/B 都必须同时记录可见瓦片数、数据 zoom、feature/三角形数量和失败数；只看 FPS 可能把“少画内容”误判为优化。

## 故障判别：先定位分叉阶段，不先改参数

| 现象 | 首要假设 | 支持证据 | 最小证伪/定位动作 |
|---|---|---|---|
| 横向错位、瓦片拼接成条带 | H1：scheme/Y/scale/GCJ 或 manifest 绑错 key | 邻瓦边界整体平移、同一 class 与 amap.com 位置差常量 | 打印 `{request key, manifest group/id, tile rect, rawY, scale, lng/lat}`；用相邻瓦共享边界点做数值对拍 |
| 部分多边形破碎/孔洞填反 | H2：环方向、canonical Y、越界 CDT/clip 不一致 | 破损集中在孔洞、凹面、跨瓦边界或复杂大面 | 运行 even-odd/concave/disjoint/real-sample triangle centroid tests；比较 CDT 前后 mask |
| 水系被公园/植被盖住 | H3：source 责任或 paint order 重复 | 30001 同时来自 main 与 water/regions，或未知 class 默认陆地色 | dump 每层 `{source,class,kind,paintOrder,z}`；确认 main 30001 透明、water12 仍 active |
| 工业园/篮球场在江里 | H4：点/线与面使用了不同 Y/scale/GCJ，或 part type 被误判 | 点/线偏移量与邻瓦或同瓦水面一致 | 用同一个 tile key 导出 POI、道路、水面首尾经纬度；先验证坐标，再验证样式/遮挡 |
| 拉远时空洞或父子重影 | H5：replacement 单元不完整即交接，或 render set 有 ancestor pair | activeAncestorPairs>0 或旧 active 先 drop | `LodSwapIsAtomicNoHoleNoOverlap` + 逐帧 `{render,active,ready,pairs}`；旧占位者必须在 unit ready 前保留 |
| 高空交互卡顿/收敛慢 | H6：网络两跳、decode/tess 队列、重复工作或 commit 堵塞 | `commit≈0` 而 tess queue/work 高，或 raw fetch/refetch 异常 | 固定相机采集七阶段指标；分别 A/B worker，不减少全球瓦/LOD；先证明瓶颈再改 |
| 日志显示 pending=0 但 CPU 仍忙 | H7：逻辑取消而非真实取消 | 旧 viewEpoch 任务仍有 work/queue | 记录 cancel、旧任务丢弃数、worker workMs；若 work 继续增长，A-L6 仍未闭环 |
| 无报错但标签/瓦片停在半程 | H8：按需渲染未申报 Landing/Pumped，或 hidden source 未 suspend | inbox/ready 非空但帧已 idle | 触发 `suspend()`/唤醒票；确认结果“先入箱再释放票” |

排障顺序固定为：**请求身份 → tile scheme → raw Y/scale → GCJ → ring topology → source filter → paint order → LOD handoff → queue/commit**。不要通过加大 `maxTilesPerView`、关闭 POI、隐藏水系或直接降低 zoom 来掩盖位置/层级错误。

## 验收矩阵

### Host/native

最小回归集合（脚本入口均为 `cd scaffold && ./test_native.sh <target>`）：

| 目标 | 测试目标 | 主要保护 |
|---|---|---|
| 请求协议 | `test_amap_tile_manifest` | zoom 映射、版本、POST body、URL group/id 精确匹配 |
| 容器/字段 | `test_amap_vector_tile` | building/line/region/type4、坏 header/gzip、POI 专用解码、真实样本入口 |
| 坐标/拓扑 | `test_amap_geometry` | scale、POI anchor、Y flip、GCJ、even-odd、孔洞、凹面、disjoint、clip、三角形 mask |
| source 装配 | `test_amap_vector_source` | Amap scheme、regions filter、共享 type1 cache、pipeline |
| Tree/LOD | `test_vector_tile_tree` | z3 全球 64 瓦、降档、反经线、祖先回退、GeometryReplace、无洞无父子对 |
| 异步链 | `test_mvt_vector_source` | pool 隔离、cache L1/L2/inflight、生命周期、失败退避、旧 mesh 丢弃、原子换手、commit budget |
| 设备策略/样式 | `test_mvt_basemap_grading` | worker budget、建筑/道路分级、Amap vector/satellite/road-overlay 编译期开关与 raster overlay 装配 |

典型命令：

```bash
cd /Users/yan/Desktop/work/gis-md/scaffold
./test_native.sh test_amap_geometry
./test_native.sh test_amap_vector_tile
./test_native.sh test_amap_vector_source
./test_native.sh test_mvt_vector_source
./test_native.sh test_vector_tile_tree
./test_native.sh test_mvt_basemap_grading
```

真实样本测试只有在设置 `AMAP_SAMPLE_TILE` 等环境变量时才会执行；未设置时的 skip 不能写成真实服务回归已通过。

### PHK110 真机

- 只用 PHK110 作为本轮高德真机证据；V1818T 的低内存行为另记，不能用旧 APK 或另一设备日志替代。
- 至少验收：全球高空完整覆盖、重庆近景 water12/main/POI 全部收敛、连续缩放无洞无重影、快速平移旧 mesh 不回写、失败/恢复不连坐。
- 确认一行 `MvtWorkers split ... model=...` 和每 120 帧 `AmapSource`/`MvtPool` 汇总；没有这些日志的“变快/不卡”结论不入北极星。
- 视觉异常仍需截图时只截当前视野和固定机位；已有 host 数值测试能确定的问题不要求截图。

## 禁止的“优化”与错误修复方式

- 不得为了 PHK110/V1818T 直接减少全球 z3 覆盖、把高空固定到中心最近 256 瓦、关闭必要图层或把 z14 细节永久降到 z12。
- 不得把 `maxPendingRequests`、worker 数、每 source commit 数混写成一个“瓦片上限”。它们分别控制网络在途、CPU 并发和 GPU finalize 节奏。
- 不得把 `layerRules` 当成整层白名单；要排除层必须设置 `includeLayers`。
- 不得把 manifest 返回数组第一项当当前瓦；必须匹配 group/id。
- 不得通过二次翻转 Y、把 GCJ 偏移应用到 tile-local、或用 XYZ scheme“看起来接近”来修视觉错位。
- 不得用默认陆地色吞掉未知类别；未知 class 应透明并记录。
- 不得在 worker 触碰图集、GL 或地形采样器；不得在渲染线程做大规模 protobuf/多边形 CDT。
- 不得为消除父子重影恢复已撤销的 POI“全有全无”回退；连续几何和符号的换代代价不同。
- 不得把“旧任务结果被丢弃”写成“旧任务被取消”；在 A-L6 实现前，真实网络/CPU 成本仍存在。

## 已知债务与开放决策

1. **真实取消（A-L6）**：需要跨 HTTP、cache、decode、tess 的 token 和 exactly-once 终态；优先级高于继续调 worker 数。
2. **全链 commit budget（A-L5）**：把四个 source 聚合到一个主线程 `commitMs`/item 视图；决定 R* 单元超额时是延迟、预合并还是可证明地拆分。
3. **全球边界回归（A-V2）**：补跨反经线、极区、全球低 zoom 和邻瓦共享边界点的自动化 fixture。
4. **服务端版本兼容**：manifest/POI schema、type3 height 字段和容器字段放宽策略需要动态版本样本；当前不能保证服务端升级后仍完全兼容。
5. **无数据与失败分流**：异步 fetch 需要区分合法空地面、manifest 不匹配、HTTP 失败和容器损坏，避免用同一失败退避掩盖服务端无数据。
6. **复杂区域成本**：越界区域仍可能经历 CDT→clip→渲染层 tessellation 两次几何工作；优化应先量化 `convertMs/tessMs`，不能先删拓扑保护。
7. **观感终验**：真实 amap.com 逐视角对照、近景水系层级、工业园/篮球场等 POI 空间关系仍需要固定样本集和 PHK110 人眼确认。
8. **Cesium 对齐复核**：上游目录缺失前，只能引用本仓库已有对齐注释/测试；恢复上游源码后再把候选契约升级为已核实证据。

## 证据索引

### 运行时链路

- [高德装配、source 分层、fetch 三阶段与日志](../../scaffold/examples/android/MinimalGlobe/GLESView.cpp)
- [worker budget、URL/缓存容量和 demo 开关](../../scaffold/examples/android/MinimalGlobe/MinimalGlobeDemoConfig.cpp)
- [预算常量、Amap key/referer 配置和缓存容量](../../scaffold/examples/android/MinimalGlobe/MinimalGlobeDemoConfig.h)
- [manifest、版本探测、group/id 选择和 data zoom](../../scaffold/src/earth_engine/data/AmapTileManifest.h)
- [Amap 4326 scheme 与 tile rectangle](../../scaffold/src/earth_engine/tiling/TileScheme.cpp)
- [raw/decoded 两层 cache、inflight、失败退避](../../scaffold/src/earth_engine/data/MvtTileFetchCache.h)
- [Tree 选择、离散 zoom、祖先回退、GeometryReplace](../../scaffold/src/earth_engine/data/VectorTileTree.h)
- [source update、inbox、epoch 闸、tessellation 和 R* commit](../../scaffold/src/earth_engine/data/MvtVectorSource.h)
- [Nebula 解码字段、POI 替换和 type/part 语义](../../scaffold/src/earth_engine/data/AmapVectorTile.cpp)
- [scale、Y flip、GCJ、ring normalize、clip 和 Feature 转换](../../scaffold/src/earth_engine/data/AmapGeometry.cpp)
- [regions/main/water12/POI 的过滤和 source 类型](../../scaffold/src/earth_engine/data/AmapVectorSource.h)

### 行为规格与测试

- [通用矢量北极星](vector.md)
- [MVT 通用架构与扩展边界](../mvt-vector-architecture.md)
- [高德 manifest 测试](../../scaffold/tests/unit/data/test_amap_tile_manifest.cpp)
- [高德容器测试](../../scaffold/tests/unit/data/test_amap_vector_tile.cpp)
- [高德几何测试](../../scaffold/tests/unit/data/test_amap_geometry.cpp)
- [高德 source 测试](../../scaffold/tests/unit/data/test_amap_vector_source.cpp)
- [Tree/LOD 测试](../../scaffold/tests/unit/data/test_vector_tile_tree.cpp)
- [MVT source 生命周期/缓存/换代测试](../../scaffold/tests/unit/data/test_mvt_vector_source.cpp)
- [高德 worker/style 分级测试](../../scaffold/tests/unit/data/test_mvt_basemap_grading.cpp)

## 更新协议

每次高德链路改动收官时：

1. 在本文判据表中更新状态、证据和代价；不要只改代码注释。
2. 写清改动影响的 source、线程阶段、坐标/拓扑不变量和可能的故障现象。
3. 至少运行受影响的 focused native tests；涉及全链或共享 cache 时追加 `test_mvt_vector_source`、`test_vector_tile_tree` 和 `test_mvt_basemap_grading`。
4. 涉及视觉或性能时，在 PHK110 固定场景采集日志；截图只用于人眼判定，数值可确定的问题优先用测试。
5. 任何新增的“快”都必须同时报告：可见瓦片/LOD 是否改变、网络请求数、decode/tess work、commitMs、内存和失败数。
6. 若某项结论只来自本仓库二手对齐或一次真机观察，明确写“候选/待复核”，不要升级为硬规格。
