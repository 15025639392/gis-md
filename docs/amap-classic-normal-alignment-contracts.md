# AMap classic-normal 对齐合同

> 2026-08-30 官方唯一入口加固：Release 的逐 scope 样式 installer 与
> identity/dash selector 已从公共 API 移除并收进私有合同；仅测试构建保留
> 畸形合同适配器。AMap runtime 与 generic VectorLayer/FeatureRenderLayer/MVT
> 禁止共存；Android `+ Vector` UI/JNI/实现已物理删除；纯矢量 AMap 配置不再
> 创建无 source 的 TerrainPageStore/raster 生命周期。
> Android MinimalGlobe 现已进一步删除 `kEnableAmapVectorDemo` 与
> `debug.ee.amapvector` 启停分支：Release 不再能绕开 official runtime，
> 字形只允许由 runtime 私有的官方 SDF demand 进入；不存在
> 关闭 AMap 后改走 generic font/style 的生产双路径。
> 测试构建中的六个逐 scope 样式安装器也已物理删除；生产与测试现在都只
> 能消费完整 `Main/Regions/Poi` sealed profile。TerrainPageStore 的
> enabled-but-not-instantiated 状态也进入互斥检查，封闭首帧前安装 official
> 后下一帧再创建 PageStore 的时序双路径。
> SDK generic MVT 安装也不再调用空 `setStyle`：新层直接使用其 generic
> 构造默认值，不保留“安装时经过样式入口”的旧动作。平台集成测试同时锁定
> official→generic MVT 与 generic MVT→official 两种安装顺序都必须原子拒绝，
> 失败时既不改变 official runtime，也不删除既有 generic source。
> 官方运行时与 generic icon 现在也双向互斥，避免共享单页 atlas 的容量被
> generic 资源消耗后饿死按需下载的官方图标；只读 Renderer 链上的 icon/glyph
> atlas 纹理 accessor 返回 `const Texture*`，不再允许调用方绕过官方资产入口
> 直接覆盖像素。
> Engine/Scene/SDK 边界同时封闭 raster 反向注入：已有 raster overlay 或
> TerrainPageStore 时拒绝安装 official runtime；runtime 激活后拒绝 SDK
> scene replacement、重新启用 PageStore，以及任何携带 raster overlay 的
> primary/staged/content Tileset。SDK 通过 private Engine 入口安装唯一 Scene
> terrain Tileset；它可以是 ellipsoid 或 Heightmap，但必须无 raster，且只经
> official land decorator 改变底色，不改变高度或官方矢量合同。调用方无法
> 复用该入口注入第二套几何或样式路径。
> 同轮继续物理删除了 `kEnableVectorDemoLayers=false` 下残留的 generic
> FeatureRenderLayer/edit/cluster 源码、Java/JNI 和触摸分支；官方 runtime
> teardown 现在清除 `amap-icons-*` frame 与字体派生 glyph，后续 runtime
> 不能在资源下载失败时复用上一代资产。CDT 在任何 predicate 前按
> `points+3` 与平面三角网安全容量预留 point/triangle storage，保持所有
> 遍历、浮点运算和输出顺序不变并消除确定性的 vector 整表搬迁。
> 官方真实数据门禁不再依赖本机环境变量：仓库固定携带 56 张北京、上海、
> 深圳、东京、乌鲁木齐跨区域 type-1/type-2 PBF，以及 type-4 region、POI、
> road-name、matching road 与 parent/child surface 样本；默认 CTest 会全部
> 解码并验证 official identity/style/geometry/source commit，零 skip。
> 同一 `emulator-5554` Release 的 149 条官方慢瓦记录现已验证该预留：
> 85 条 regions 样本累计 `648,120,650` 次 point-triangle predicate，最大
> `polyInput=10069`、`cdtPeakTris=13037`，point/triangle capacity growth
> 合计仍为 `0/0`。日志汇总器也已兼容 Android `-v brief` PID 前缀，避免
> 把有效官方证据误判为空。
> Bowyer-Watson point insertion 的全扫描现进一步取消了每次 predicate 前
> 对 `Tri` 的无条件值复制：扫描阶段没有 `push_back`，并且稳定压缩恒满足
> `keptCount <= read`，因此可只读引用当前槽位，仅在 survivor 跨越真实空洞
> 时复制。谓词参数/次数/顺序、bad-edge 顺序、triangle 顺序和 indices 不变；
> CDT 9/9、Polygon 13/13、FeatureRenderLayer 176/176 与 Android Release 通过。
> cavity scratch 也不再为每个坏三角形分别写三条边：现在只保存一次原
> `Tri`，后续两遍仍严格按 `(v0,v1),(v1,v2),(v2,v0)` 展开，保持 directed
> hash 插入/反向查询和新三角形发射顺序。按已采集官方样本的 464,622 个
> bad triangles，scratch push 从 1,393,866 次降为 464,622 次，写入约从
> 10.6MiB 降至 5.3MiB；不减少可见几何，也不改变任何 predicate。
> 同一模拟器冷启动后的 19 个完全匹配官方瓦片进一步确认：两次运行均为
> `142,243,209` point-triangle tests 与 `425,450` bad triangles；聚合
> point insertion 为 `58.858ns/test → 59.815ns/test`。该 `+1.6%` 在并发
> worker 调度噪声内，不能证明 wall-clock 加速，因此这里只认定确定性内存
> 写入减少，不把它包装成速度收益，也不继续叠加缺少测量收益的微优化。

本文件是纯矢量地图与 `amap.com` classic `normal` 样式对齐的唯一进度清单。
后续不再以零散截图现象作为工作队列；每次改动必须归属一个合同，并更新其
官方证据、实现位置、自动测试、Android 证据与状态。

固定官方 PBF 的字段级完整性另由
`scaffold/tools/extract_amap_style_contract_inventory.py` 直接扫描并输出 JSON：
当前覆盖 Style `#1..#11`、Label `#1..#17/#20/#21`、Road `#1..#9`、
Region `#1`、Guide `#1..#8`、Building `#1/#2` 共 50 项。每项都必须具有
分类及 extractor→generated/runtime→C++ 最终 consumer 链；观察到新字段、
字段集合漂移或 consumer 符号消失均测试失败，不允许继续靠人工 PASS 掩盖。
其中 Style `#10 fresh` 与 `#11 continuation` 已拆为“道路字段显式 reset”与
“跨记录 identity/window 连续化”两条不同状态机合同；Label `#16` 必须先验证
官方临时尺寸公式、再由实测动态文字布局覆盖，`#17` 分别锁定 bit1 的碰撞行为和
bit4 的官方无渲染 consumer 结论；Region `#1` 则一直追到 command 的
`vectorUniforms.color`，
不再以附近存在泛化 fill style 符号作为通过依据。
Style 顶层 `#5..#9` 也不再共用泛化 `consumed`：Label、Road、Region、Guide、
Building 五个容器分别追到 label size、line width、fill command color、guide
icon demand 与 extrusion command 的唯一最终路由，删除任一路由会按原字段编号
fail-loud。
Style 顶层 `class/subKey/minZoom/maxZoom` 不再共享泛化的字符串存在证明：四项
分别锁定 identity class、identity subkey、窗口下界和窗口上界，并横跨
POI/road/surface 三类生成器及对应 C++ selector。负向变异测试会在删除任一
专属 final consumer 时按具体字段 fail-loud，防止“别的字段碰巧含同名文本”
造成假通过。
Label 文字字号/颜色/halo、Road 九个 stroke/label 字段、普通图标与动态背景
的 atlas/index/cell/display/atlas-size、Guide 八字段以及 Building roof/wall
也均已拆成字段专属语义链。图标链继续穿过官方整图尺寸校验、一基 cell 裁切、
frame 上传和 CSS 布局；Building 链落到独立 roof/wall extrusion color map。
审计测试会在内存中分别移除 halo、casing color、atlas height 或 wall color
consumer，并要求错误精确归属原字段，不能由邻近共享代码替代通过。

## 状态定义

- `PASS`：官方结构化证据、实现、聚焦测试和适用的 Android 证据齐全。
- `PARTIAL`：已有正确实现，但身份、缩放窗、边界或端到端证据不完整。
- `FAIL`：已证实与官网合同不一致。
- `UNKNOWN`：尚未提取足够证据，不得按经验填值。
- `N/A`：官网合同不适用于当前纯矢量目标，必须写明原因。

完成一项合同至少需要：官方 PBF/runtime 证据、源码落点、聚焦行为测试。
涉及上屏行为时还需要 Release Android 证据，并确认 `rasUp=0`。

## A. 数据与版本合同

| ID | 合同 | 官方证据 | 本项目落点 | 验证 | 状态 |
|---|---|---|---|---|---|
| A01 | style PBF 版本与 SHA 固定且可重放 | official PBF SHA `e7e722...adca` | `.codex/artifacts/amap-label-probe-20260829/` | `test_generate_amap_style_data.py` 从固定 PBF/runtime 重建 surface/road/lineType/POI 四表并逐字节比对提交产物；错误地把 WebGL bundle 当 lineType runtime 会 fail-loud | PASS |
| A02 | 生产瓦片版本探测、URL group/id 精确绑定 | JSAPI runtime + manifest response | `AmapClassicRuntime` 私有 transport + `AmapTileManifest.*` 纯协议 helper | focused runtime transport 测试锁定 `GET /web/init → POST /web_map/get_tile → requested group/id signed GET`，type1/type2 分别只选 `building_region_road_transit` / `poi_region_road_transit`，所有官方请求携带固定 Referer；manifest parser/selector tests + Android fetch | PASS |
| A03 | type0/1/2/3/4 payload 解码字段语义 | runtime decoder + production tiles | `AmapVectorTile.*` | FeatureMulti `drawOrder/minZoom/maxZoom` 保留数值与 presence；BuildingSameStyle protobuf defaults 为 `mainKey=55001/subKey=1/resolution=12`，ShapeFeatureMulti defaults 为 `min=15/max=30/drawOrder=0`，ShapeFeature `#5` height default 为 6m且按 protobuf `int32` 符号语义解码；81 个当前官方 type-1 瓦片中的 322 个 type-3 layer 全部为 resolution 18，raw bounds 精确为 `131072×65536`；转换按官方 `getCoordShift=33-resolution-z` 等价公式进入 canonical `8192×4096`，非法 shift/超 int varint 在几何前 fail-closed；adapter 发布 building identity、resolution、feature zoom window、drawOrder 与显式非正 height，不再回落 planar fill；旧 bytes→Feature decoder、同步 demo loader 与 south-up 坐标兼容参数均已移除 | PASS |
| A04 | `class/subKey/kind/type/z` 全路径属性保真 | production identities | `AmapGeometry.*` | geometry real-sample tests | PASS |
| A05 | 未知身份透明/安全回落，不伪造样式 | official table absence | source adapters + styles | unknown identity tests | PASS |
| A06 | shared cache 不改变 decoder profile 或身份 | production request topology | `MvtTileFetchCache*` | cache/source tests | PASS |

## B. 坐标与几何合同

| ID | 合同 | 完成判据 | 当前状态 |
|---|---|---|---|
| B01 | AMap tile-local Y、4326 grid、GCJ/WGS84 转换一致 | 生产锚点落入对应瓦片且邻瓦连续 | PASS |
| B02 | Polygon outer/hole/even-odd parity | 生产面三角质心全部在源 mask 内 | PASS |
| B03 | 多环 kind surface 保持 provider 级 parity | 不拆散全局 modulo-two mask | PASS |
| B04 | LineString 点序、瓦片裁剪与跨瓦连续 | PASS（当前官方 9×9 相邻 z14 瓦片使用“同 identity + 对侧 tile edge + 对向内切线”的一对一匹配：3993 对/7986 个端点精确续接，最差误差 `0 canonical unit`；67 个未配对端点集中在 20007/20008/20009/20012/20017，其中 36 个最近对侧端点超过 64 units、18 个处于 1 unit 内的多分支竞争，只有 2 个方向冲突。证据排除坐标换算裂缝，剩余为 provider 边界分叉/终止，不做会误连道路的本地吸附） |
| B05 | type-1 road-name path 与道路几何关系 | 重庆真实同瓦 type-1 双 payload 数值诊断：23 条均有同 class 可见道路候选，22 条 Hausdorff=0；唯一 `建新西路` 为官方独立文字 path，与最近同 class 道路约 151.442m。生产直接消费 provider-selected path，不做本地吸附/替换 | PASS |
| B06 | source zoom 层级所有权 | 11.99/12.0 无 surface 空窗或重复 | PASS |
| B07 | 水体 source 所有权 | z12+ 全部 type2 payload 由 main 原样消费；已删除按 `subKey/kind` 人工过滤并冻结 z12 的 `water12` 第二 source | PASS |
| B08 | 建筑 footprint 与高度语义 | 官方 style field-9 + JSAPI `decodeStyleList/BuildingSameStyle/ShapeFeature/z9t/tileInnerCoord2LngLat/getCoordShift` + 当前 `26_07_27_00` 的 81 个真实瓦片 | `55001:subKey` 单一身份；25 条记录按官方 continuation/inheritance 展开；subKey 5 缺席并 fail-closed；roof/wall 独立存储和烘入顶/墙；最终消费者回归使用官方半透明 `55001:21`，证明 display `.8` min window、`0x804DA6FF` 的 RGBA8、blend/depthWrite 与 `VectorExtrusion` command 同时成立，不再只用手写白色 generic extrusion 证明命令存在。高度仅取 ShapeFeature #5，缺失为 6m，显式值按 protobuf `int32` 符号扩展，0/负值保持 presence 后零 extrusion/零 planar fill；resolution 12/13/18 分别 scale 4/2/1/16，非法 resolution 在坐标转换前拒绝；旧 10m fallback、generic empty-map extrusion fallback 与生产隐藏路径已删除；Release emulator 前台运行、无 fatal/GL error 且 raster-zero | PASS |

## C. Surface 面样式合同

| ID | 合同 | 官方键 | 当前状态 |
|---|---|---|---|
| C01 | 30001 全 subKey 颜色、alpha、zoom window | field 7 `subKey × displayZoom` | PASS（command-time 晚绑定；未知 subKey 透明；selector 不组合 caller/legacy expression） |
| C02 | 30002 全 subKey 颜色、alpha、zoom window | field 7 `subKey × displayZoom` | PASS（command-time 晚绑定；未知 subKey 透明；未知 class 固定进入透明 group 0） |
| C03 | 植被、水、海洋身份不混用 kind/subKey | 30001 identities | PASS |
| C04 | 足球场/篮球场/同族场地换色 | sub19/20/21，`.8` 阈值 | PASS（同一 GPU mesh 在 `.8` 阈值仅切 command uniform） |
| C05 | 30003 高缩放 overlay 全 subKey/alpha | field 7 | PASS（官方 onset 前不发 fill command） |
| C06 | 面 paintOrder 与覆盖关系 | surface → building → transport | PASS（`drawOrder` 只排序；surface/building 视觉 identity 均由官方 class/subKey styleGroup 决定；extrusion range 保留 styleGroup，不再按 paintOrder 合并不同建筑颜色；跨 kind 全局排序由 H04 的 command-order 回归覆盖） |
| C07 | 未知面不回退通用蓝色 | missing metadata → transparent | PASS |
| C08 | surface source/display zoom 分离 | source z14 可响应 display z15+ | PASS |

## D. 普通道路 stroke 合同

每个道路身份的合同键为：
`class/subKey × displayZoom → center(width,color,lineType,cap) + casing(width,color,lineType,cap)`。

| ID | 范围 | 当前状态 | 剩余工作 |
|---|---|---|---|
| D01 | 20001 全 subKey | PASS | 保持生成表回归 |
| D02 | 20002 全 subKey | PASS | 保持生成表回归 |
| D03 | 20003 全 subKey | PASS | 保持生成表回归 |
| D04 | 20004 全 subKey | PASS | 保持生成表回归 |
| D05 | 20007 全 subKey | PASS | 保持生成表回归 |
| D06 | 20008 全 subKey | PASS | 保持生成表回归 |
| D07 | 20009 全 subKey | PASS | 保持生成表回归 |
| D08 | 20012 全 subKey | PASS | 官方生成 identity/curve |
| D09 | 20013 全 subKey | PASS | 官方生成 identity/curve |
| D10 | 20018 全 subKey | PASS | 官方生成 identity/curve |
| D11 | 20023 全 subKey | PASS | 官方生成 identity/curve |
| D12 | 20030 全 subKey | PASS | 官方生成 identity/curve |
| D13 | styleGroup 优先级不被 paintOrder 近似覆盖 | PASS | 旧普通道路 paintOrder stroke 表已移除 |
| D14 | `.8` fractional zoom selector | PASS | width/color/type/window 全覆盖 |
| D15 | DPR 只缩放 CSS pixel，不改变身份/zoom | PASS | Android density + unit tests |
| D16 | center/casing 独立 dash 和 cap | PASS（官方 `getLineTypeStyle` 的 0–16 全状态直接驱动唯一 lineType consumer；当前生成曲线实际可达的 14 实线 round 会从同一 curve 自动准入 endpoint quad，13 square 能力保留但当前 style PBF 无引用；15/16 dash cap 同理由 resolver 精确保留。端帽 quad 携带真实相邻点以建立首尾切向，GLSL/MSL 对 solid butt/square/round 分别执行丢弃/方形/圆形裁剪，不维护第二张手写 cap identity 表） | 保持生成表与跨后端 shader 回归 |
| D17 | 高 zoom 宽度扩展不新增几何组 | PASS | command-time expression |

## E. 特殊线与交通合同

| ID | 合同 | 当前状态 |
|---|---|---|
| E01 | 20010 railway sub1/2/3 stroke + label | PASS（视觉键直接使用官方 `2001001..2001003`，label drawOrder 独立） |
| E02 | 20011 ferry dash/color/width | PASS |
| E03 | 20014 guide/boundary line | PASS（已移除本地 `6401/64` ordinal；当前 81 个真实瓦片中 `20014:1` 32 条、`20014:2` 52 条均可达。RegionLayer `content.#2` 只拥有官方线几何与 class/subKey，不拥有 road-name schema 的 name/rank；因此 sub2 的官方 width 为零时严格不生成 stroke，也不借 line-label 样式表伪造区域文字。文字只由 request-type-2 的官方 `road_name` payload 生成） |
| E04 | 20015 transit route 全 subKey 颜色 | PASS |
| E05 | 20015 route width/casing/color/zoom 扩展 | PASS（186 个官方 subKey 的宽度、外描边和颜色全部使用无碰撞 `(class, subKey)` styleGroup；已删除 `amap_linekey` 本地视觉复合键） |
| E06 | 20016 boundary network 全 subKey | PASS（已移除本地 `650x` ordinal，直接使用 `20016xx` identity） |
| E07 | 20017 boundary casing 全 subKey | PASS（已移除本地 `660x` ordinal，直接使用 `20017xx` identity） |
| E08 | 20019 under-construction semantics | PASS（当前 81 个真实瓦片中 `20019:1` 有 30 条生产 path；已删除本地透明 center+1px casing-only 语义，恢复官方 z15+ center width 2/4/...、`#bebebe`、lineType 4 dash 与 10px 灰字/白 halo；仓库无 `90003` 别名） |
| E09 | 特殊线 paintOrder 与普通路互不串色 | PASS（普通路、铁路、guide、boundary、transit 共用封闭的官方 tuple selector；API 不再接受 caller fallback，未知 tuple 固定为 0） |

## F. 道路文字合同

| ID | 合同 | 当前状态 |
|---|---|---|
| F01 | field 5 字号、颜色、halo、zoom window | PASS |
| F02 | type-1 road-name 保留源 road class/subKey/rank/window | PASS（不再合成本地 `90002` class；本地 `amap_payload_role` 合同及 renderer 分支已物理删除，是否准入道路文字只由官方 road class/subKey label identity 决定；rank/window 缺失或畸形均 fail-closed） |
| F03 | 路名沿路径逐 glyph anchor | PASS |
| F04 | 官方屏幕空间路径切段、阅读方向与折点 tangent | PASS（固定 `LabelLine.Zp` 的 `zoom<12 ? 10 : 70px` 激活阈值、`fontSize*1.3` 段单位、`π/10` 转角切断、方向翻转切断、`0.6` 短段阈值、长首段裁切和 `labelsUtil.Ed` 阅读方向翻转均已落地；全部候选段进入生产。固定 `Yp/zd/Ud` 从首候选起点逐字前进，后续候选先执行 `zd`，相邻 glyph angle 必须 `<π/10`，完成或拒绝后均跳过 `Wgt + glyphCount*e1 = 300 + glyphCount*30px`，最多 `qgt=60` 次。每个 candidate/group 独立烘焙、碰撞、淡入和稳定 identity，按官方生成次序参与 stamp 优先级。CSS 常量按 provider DPR 恰好转换一次。旧“整条路径跨急弯”“只选最长候选”“每候选中央只排一组”均已从官方生产路径物理删除；数值测试覆盖切段、阅读方向、多组间距、后续候选初始 skip、60 次上限、短候选拒绝与 DPR） |
| F05 | readable direction / reverse geometry | PASS |
| F06 | 路径简化误差有界 | PASS（删除本地固定 65 点 RDP；type-1 road-name 完整保留官方 provider path，ProjectedPathSampler 直接消费全路径） |
| F07 | 地表弧长与屏幕弧长在俯仰透视下等价性 | PASS（固定相机 A/B 证明旧地表半弧长锚点偏差 >8px；生产沿路文字已改用 `ProjectedPathSampler` 的 clip-w 透视正确屏幕弧长，数值容差 1e-6px；相机导致样本路径漂移 >2 framebuffer px 时才刷新，每帧最多原子重烘一个瓦片桶，新 VBO 全部成功前保留旧标签） |
| F08 | 曲线标签碰撞盒覆盖真实 glyph 曲线 | PASS（每个已绘制 glyph 的 ECEF anchor/tangent 与实际 bitmap+halo 局部盒直接进入 placement；沿线文字不再使用中心整行矩形 fallback；数值测试验证 glyph 间真实空隙不阻挡、命中 glyph 则冲突） |
| F09 | repeat distance、padding、最大弯角 | PASS（官方 `NebulaLabelFormat.DQ` 逐条透传 provider 已选定的 roadName `path/distance/rank/zoom`，生产已删除本地 `220px` 同名抑制和 `35°/180°` 折点二次拒绝；官方 field-5 `[0,1,0,1]` padding 与 `BO/JO` 的 `CONSTS.ic=1px @ 24px` 已进入按 label styleGroup 索引的官方 runtime layout record，official worker 不再读取 layer-wide generic `lineLabel*` 槽位） |
| F10 | 少数 type-1 road-name path 与可见路不重合的绑定策略 | PASS（官方 `handlerTileRoadLines` 将 provider-selected road-name line 作为独立 label path；真实样本 22/23 与道路完全相同，唯一独立 path 仍按官方 payload 直接使用；不引入最近道路吸附、重投影或名称猜测第二路径） |

## G. POI、行政文字与图标合同

| ID | 合同 | 当前状态 |
|---|---|---|
| G01 | POI/行政 field 5 继承语义 | PASS（固定 PBF 的 point-style `flags` 已完整生成并按 identity+display-zoom 有序查询。bit1 `canCovered` 严格对齐官方 Ff→Ef placement 消费链：所有盒先进搜索树，因此它仍可被已处理的更高优先级普通标签拒绝；轮到自身时跳过 icon/text search，不主动淘汰低优先级标签。本地唯一路径等价实现为“查询已接受的高优先级普通盒，但自身不写入 accepted grid”。官网当前实际版本仍为 2.3.5.6；完整主 bundle 同时包含 LabelPlacement/LabelBucket/LabelTextStyle/LabelWorker，bit4 `Y8t` 全部 7 次引用仅用于 decode/forward/merge，无 placement、碰撞、SDF、VBO 或 draw consumer。因此本地仅在官方 resolver 边界解码它，不再把无可观察效果的字段复制进 label source、entry 或 placement candidate；extractor 对引用数量及完整 consumer 存在性 fail-loud） |
| G02 | 字号、颜色、halo、offset、zoom window | PASS（字号/颜色/halo 使用官方 tuple identity；halo width 也已进入按 styleGroup+zoom 求值的官方 runtime 表，SDF 描边与 collision box 共用同一解析宽度，缺表/坏值 fail-closed，不再读取 generic `labelHaloPx/labelSizePx`；AO/XV/BO/EO/TO 已落地：POI/Guide 的官方 `[top,right,bottom,left]=[0,1,0,1]` 在 AO 中表现为 Right/Left 1 CSS px 横向间距、Top/Bottom 0 CSS px 纵向间距；BO 多行 gap 为 3 CSS px，字形几何与碰撞盒共用同一行步进，DPR 恰好应用一次；EO 文字碰撞盒只左右各扩 1 CSS px、上下不扩，icon 碰撞盒严格使用官方 frame 原尺寸；已删除旧固定 3px icon gap 和四向 3px collision inflation。PointFeature repeated `nameLoc` 默认严格选择第一条；Language repeated field-4 `Mii` 作为独立 UTF-16 split indices 贯穿 decoder→Feature→TileSymbol→label bake，名称原文不插 `\n`，因此不污染名称特例、cross-tile ID 或重复 identity。固定 runtime `getSpiltLineWithSpiltIndex` 在消费全部 index 后始终追加剩余 `substring(n)`，所以最后一个 split 不必闭合到字符串末尾；本地已删除旧“末项必须等于总长度”路径。官网最终 `j8t/tQ` 仅在 `JQ/Mii` 非空时调用 helper，因此空 split 不进入 helper 的 `trim()` 分支；非空 split 在追加 suffix 后只执行尾部 ECMAScript `/\s+$/`，保留前导空白。本地严格在临时布局行执行同一尾裁剪，不修改 provider name、cross-tile ID 或 stored identity。split 非递增、越界或切入 surrogate pair 仍 fail-closed。固定 Shanghai q8t `B-128弄39号` 的 `[8,9]` 形成两行，Tokyo fixture 默认保留首条 `东京` 而不再被末条 `東京` 覆盖。旧 `layoutResolved` 布尔选择器及散落二维字段已删除，AMap resolver 只发布独立 `ProviderLabelLayout` payload；当前 2.3.5.6 `Style.Poi/R9t` 不发布 `nn/an`，因此非 `icons_9` 严格按官方 `u.nn||0/u.an||0` 进入 Igt 数值 top-left 公式，而不是保存手写 `10/10` anchor；`icons_9` 则严格消费官方字符串 anchor `bottom-center`，以 tagged anchor 贯穿可见 quad、icon collision box 和文字相对布局，不再错误进入数值 Igt 公式；CSS frame 尺寸留在 CPU 顶点，官方二值 retina uniform 在 shader 恰好应用一次 DPR。`CITY_SPECIAL_CONFIG` 与 `TEXT_DIRECTION_STRATEGIES` 由固定官方 JS bundle 生成，不在 C++ 重复手写；所有 27 条 alternate artwork 均按 R9t 进入 q8t：18 条 field 16/`w7t=2` 按 `floor(alternateCell/w7t)` 形成临时尺寸，9 条未发布 `w7t` 则保留原 cell 尺寸；Igt 对 named q8t 随后以实测文字布局覆盖最终 draw/collision geometry。extractor/generator 对两条分支、正值和公式 fail-loud，生产表不保存无最终作用的临时状态；q8t 最终按实测 `maxWidth+4 × (lines*(fontSize+4)+4)` 生成独立 atlas 背景 command，与文字共享 placement opacity） |
| G03 | rank、同 rank 顺序与每瓦候选预算 | PASS（POI 与道路文字只透传 payload rank；已删除本地 `classCode → -19000/-16500/-14000/-8000` 碰撞优先级表。官方 worker 的 `Util.stamp` 单调递增，主线程 `Ef` 对 rank 分组的数字键先按 ECMAScript 整数属性规则升序枚举、再倒序消费，因此同 rank 明确为较大 stamp 优先；生产在 sealed tile 准入时分配全层单调 `officialInsertionOrder`，跨瓦片、zoom 重建、地形重钳和 glyph 重烘均显式携带，不再把 `unordered_map` 收集下标、camera distance 或 featureId hash 猜成官方决胜字段，旧 `officialReverseInsertionTieBreak` 布尔/下标路径已物理删除。AMap provider 不再应用通用 `16/32/64/128` 每瓦截断，官方 identity 全部进入 rank/window/collision 合同；通用非 AMap 图层仍保留引擎保护预算） |
| G04 | 官方 icon atlas URL/version | PASS（当前官网与生产共用同一 `/web/init` 响应中的官方 icon `v/p/t`；不再为 icon 单独重复探测 init。完整 atlas/frame taxonomy 仍只由 field-5 生成，但网络请求改为当前可见官方 POI resolver 发现缺 frame 后精确 demand 对应 atlas；当前官网初始视口实测只请求 `icons_1/4/9/64`，生产不再启动时预下载完整 17 atlas，也无第二份硬编码列表。固定 PBF 中 10 条 `labelType/W9t=200` record 不发布 atlas/cell geometry，官网当前 manifest 的 `icons_200` 也返回 404；生产严格 fail-closed，不套 runtime generic 64px 默认、不合成本地图标） |
| G05 | identity → atlas/frame 映射 | PASS（全部官方记录逐 frame 携带 atlas/index/cell/atlas 尺寸与 zoom；原始合同会拒绝非法、越界或同 frame 尺寸冲突；atlas50 `48×48` 不再走旧 `64/8` 切片） |
| G06 | 缺 icon 下载并严格 late-bind | PASS（resolver 先精确 demand 官方 fixed/dynamic atlas；最终消费者回归证明 Guide `40001:110100` 由 field-8 精确 demand atlas 1，资源未到时 point quad 与裸文字 command 均不发布；任一被引用 frame 到达后同一轮原子恢复，不再短暂显示裸文字或合成占位） |
| G07 | 未知 icon 不回退伪造形状 | PASS（AMap point layer 显式选择 `OfficialProvider` 单一合同；运行时不再用 resolver 是否存在来猜测模式，也不读取通用 `pointImage/pointColor` fallback） |
| G08 | DPR 下 icon/text CSS pixel 尺寸 | PASS（官方 icon quad 保存 CSS-pixel 绝对尺寸，command uniform 只乘一次 DPR；官方安装器清除通用 `pointSizeExpr` 路径） |
| G09 | icon anchor、文字 offset、碰撞盒 | PASS（当前生产 `access_oversea=1` 作为生成参数固化，并校验官方 JS 确实存在对应 kgt 第四参数分支：有 icon/q8t 时统一 bottom `[0,0]`，名称特例仍具有最高优先级；无 icon 时严格使用官方缺省 `nn/an=[0,0]`；已删除本地 right `[0,-2]` 近似。Igt anchor 只由官方公式计算；文字几何使用 AO/XV/BO，多行文字盒使用 EO/TO block 尺寸，icon 与文字保持独立碰撞盒） |
| G10 | cross-tile ID、fade、LOD 换手 | PASS |
| G11 | 官方 SDF glyph 位图、metrics 与 fragment style | PASS（runtime 私有按可见 codepoint 需求数字排序、每批最多 128 请求 `sdf.amap.com/getsdfdata`；严格验证 PNG data URL 和 7 元 metrics，以 24px canonical size 和 `horiAdvance+1` 安装到 R8 atlas；整批只增加一次 revision。fragment 不再使用未证明等价的通用 `fwidth` 近似，官方路径精确消费 `fontSize<10 ? .78125 : 205/256`、`gamma=1.4142*(fontSize>24||DPR>1?1.7:1.5)/fontSize`、`borderBuffer=edge*(1-min(10,strokeWidth*DPR)/10.1)` 与 `buffer=edge+1.5/256*(DPR-1)`；generic 标签继续使用原 derivative 合同。APK Noto TTF、JNI installer、hash/size 识别与所有官方本地字体路径已物理删除） |

| G12 | Guide/道路盾牌合同 | PASS（固定 style PBF 顶层 field `#8` 已按官方 `Style.guide` 参与继承和生成，runtime `H9t/oV` 锁定 `styleColor/fontSize/atlas/index/cell/atlas-size`。官方 RoadLine `shield #3/shieldType #4` 由 decoder 保留，转换为 `40001:shieldType` 的单一 Point payload。定位严格重放 packed projected path 合同 `p=floor(path.length/2); position=[path[p-1],path[p]]`：奇数点路径取中央顶点；偶数点路径先把两个中央点投影到 EPSG:3857，z<13 生成 `[Y(k-1),Xk]`，z>=13 按官方 128×128 LCS cell center 减中心后交叉并恢复为固定世界锦点，再反投影回唯一 Cartographic 贴地路径。旧的相邻顶点算术中点和 raw-coordinate 交叉近似均已删除。盾牌为 `angle=0` billboard，不沿道路旋转。背景只使用官方 `icons_1` frame 114..119；宽度严格消费 `NebulaLabelFormat.oV` 的二值 `Support.scale` 分支：`T=max(1,JavaScript String.length/4)`，即按 UTF-16 code units 而不是 Unicode codepoints 计数；retina (`devicePixelRatio>1`, scale=2) 为 `24*T` CSS px，1x 为 `24*9*T/7` CSS px，高度均为 24 CSS px。五个非 BMP 字符的官方长度为 10、`T=2.5`，数值回归锁定 60 CSS px retina 宽度。旧的 codepoint-count 与无条件 `9/7` 路径已删除，CSS layout 与 shader retina scale 分阶段保证 DPR 恰好应用一次；未知 shieldType/缺 frame fail-closed，不合成图标、不走 generic shape） |

## H. Paint order 与合成合同

| ID | 合同 | 当前状态 |
|---|---|---|
| H01 | 全 class/subKey 官方 paintOrder 表 | PASS（生产 `drawOrder` 仅用于排序；视觉合同由同一官方 PBF 生成。生产只存在 Main/Regions/Poi 三个 sealed profile；未被任何完整 profile 消费的 `AdministrativeLabel` 局部 scope、installer 与实现已物理删除，公开逐 requirement 拼装入口也不存在，调用方不能构造“半官方”双路径。官方 installer 只安装 provider runtime 表，不再通过 generic fill/line/label 标量模拟关闭旧路径。sealed profile 几何准入：Main/Regions 仅 Polygon+LineString，Poi 仅 Point+road-name LineString；错类型、未知 identity、缺失窗口均在样式求值/几何分配前 fail-closed。道路按官方 drawable pass 生成 429 drawable/385 secondary-stroke/285 line-label identities；surface/building/POI 同样只使用生成合同。旧 ordinal、alias、special installer、class 白名单、POI family/range、内嵌 icon、MVT drape/RoadField/EPlan 等生产路径已物理删除） |

H01 构造边界补充：`AmapClassicRuntime` 是唯一 production owner，原子持有 official assets、私有 transport 与 `AmapClassicSourceBundle`；官方矢量身份与 Scene 地形来源正交，唯一 terrain provider 可为 ellipsoid 或 Heightmap，并统一经 official unlit land decorator 保持官网底色，不再安装第二个 flat canvas。公开 `TerrainSourceConfig::amapClassicVectorLand`、Android 预启用路径和运行中 `Engine::removeAmapClassicRuntime` 均已物理删除，构造函数与 update 均为私有，平台只能通过 `Engine::installAmapClassicRuntime` 安装，不能分别构造、pump 或销毁二者。Engine installer 不再接受 `Type1Fetch/PoiFetch`，Android 自持版本探测/manifest/签名 GET/完成队列和未使用的同步 `AmapHttpFetch/resolveTileVersion/fetchAmapTileUrls` 已物理删除；`AmapTileManifest` 只剩官方协议的纯构造/解析 helper，transport 类无公开 header，只能作为 runtime 私有 nested owner 存在。official scene 仍恒为 no raster/no generic MVT/no glTF；terrain 只负责空间高度，不改变官方 paint/layout identity。官方文字只由 runtime-private SDF codepoint demand 安装，APK 不再携带或注入字体资产；icon 则继续由 runtime 从官方 manifest/atlas 下载。runtime 现由 Scene 唯一持有并在 `Engine::render` 内推进，安装只返回 const 非 owning 诊断句柄，析构为私有且仅 Scene 的 owner deleter 可调用；Android 已删除自己的 runtime owner、视域计算和手工 update。Regions/Main/Poi 作为同一个 MVT producer 的三个 typed instance 共享 Scene 网络/worker/GPU 帧额度；official runtime 存在时 Scene 只推进 official typed sources，不再同帧调度 generic MVT。asset manifest、SDF codepoint demand 与每个 icon atlas 请求都消费 Scene 的共享 MVT network grant，每帧至多在取得一个 MVT GPU-upload grant后解码并安装一个 landed asset batch；Landing 进入 WorkLedger 唤醒。Engine teardown 先撤销共享 wake gate、再销毁 runtime，迟到的回调副本不能再访问 Engine；挂起 manifest 的取消、ticket 清零和无迟到 wake 已由测试锁定。bundle 绑定 Regions/Main/Poi sealed profile、typed AMap decoder、AMap geographic scheme、离散 `amapDataZoom`、Regions/Main 共享 type1 cache 与三源 teardown；构造期间任一 source 抛异常都会先销毁已建 source、再移除三张 official layer，故障注入测试同时锁定失败后可立即重装，不再留下半安装 runtime；原先公开的 typed cache/source/converter aliases 与 raw source/cache diagnostics 已物理删除。decoder profile 现整体属于 `AmapClassicSourceBundle` 的 private nested `Impl`：decoded tile、type1/POI traits、regions/main/POI converter、cache/source aliases 与 codec methods 同属一个访问控制边界；旧 namespace-level `decodeType1Tile/decodePoiTile/decodedPartToFeatures` 符号已消失，Release 符号表只剩无法由外部合法命名的 `AmapClassicSourceBundle::Impl::*`。生产 header 仅暴露 bundle fetch、const layer view 与复制出的 numeric diagnostics；测试 wrapper 只通过 `EARTH_ENGINE_TESTING` friend 为明确测试目标提供行为规格入口。跨 header API 污染检查与 Release `nm` 负向检查共同锁定边界；Android 也删除 raw decoder、generic MVT template 与独立 AMap style module 的旧 include 残留。profile installer 在 production 不再是 public API，standalone official layer 被 Engine 拒绝，固定 layer ID 冲突时整个 runtime 原子失败并保留已有层。official layer 的 Scene 插入与移除均收回 Engine/runtime 私有边界，公共 `removeFeatureRenderLayer` 不能再按固定 ID 拆掉仍被 source 使用的 official sink；runtime 的诊断视图只返回 const layer 与数值快照，不能从 const runtime 反向取得可写 official owner。generic `MvtVectorSource` 不能与 official layer 配对，editable `FeatureStore` 既不能渲染也不能被 official layer 拾取。官方 symbol 和烘焙后 label source 不携带 generic size/offset/icon/color哨兵槽位；三个 sealed profile 已删除本地 `2.5m` 防 z-fighting 高程偏移，官方道路、面和 POI 锚点不再被非官方标量整体抬升；`Engine::renderer()` 与 const `Renderer` 的 atlas 诊断均只返回 const 指针，外部不能绕过受控 API 直接改写 glyph/icon atlas；generic 字体一旦安装就拒绝再启用官方 runtime，官方 runtime 存在时也拒绝 generic 字体写入，因此官方标注不会借用预存 generic glyph atlas；`amap-icons-*` 命名空间只允许 official asset installer 写入。

H01 transport 边界补充：应用只可提供 web key；`Referer` 已从 public
credentials 与 Android 配置中删除，并由 runtime-private 常量固定为
`https://www.amap.com/`。init、tile manifest、signed PBF 与 icon atlas 请求
全部使用该值，调用方不能再构造非官方 transport 变体。

H01 label API 边界补充：原 production public
`AmapClassicLabelStyle.h` 已物理删除；identity encoder、POI icon/background
resolver、atlas frame/manifest 与 collision resolver 全部移入
`AmapClassicLabelStyleInternal.h`。生产调用只剩 sealed runtime 的 style 与
asset 实现，focused tests 显式引用 internal contract；API guard 锁定 public
header 不得恢复。
| H02 | 跨瓦片全局排序稳定 | PASS |
| H03 | casing 在 center 前，center 在 label 前 | PASS |
| H04 | surface/building/line/POI 跨层顺序 | PASS（所有 Vector kind 共享同一官方 drawOrder 排序域；kind 不再先于 drawOrder。仅同 drawOrder 内使用 casing=0/center=1/background=2/text=3 子序） |
| H05 | transparent pass 不产生遮挡/伪覆盖 | PASS（AMap surface 缺表/非法/alpha0 不发 fill command，且不再维护透明 group-0 sentinel；严格 AMap line 的 width0/alpha0 或 lineType 表达式缺失、非数值、NaN/Inf 时对应 center/casing phase 不发 command；数值型未知 lineType 则严格消费官方 `getLineTypeStyle` 的 `default: "solid"` + butt cap，不得误删为本地 fallback；通用顶点色路径不受影响） |
| H06 | 相同 paintOrder 下 styleGroup 不破坏排序 | PASS |
| H07 | zoom window 在 command time 正确门控 | PASS |

## I. 渲染与设备合同

| ID | 合同 | 当前状态 |
|---|---|---|
| I01 | raster/drape 关闭 | PASS（`rasUp=0`, `memImageryKB=0`；AMap raster drape provider、通用 MVT→RGBA `VectorDrapeImageryProvider`、MVT→D2/R8 `RoadFieldSource`、对应 rect coverage/rasterizer、Engine 公开 road-field 注入与换肤 API、Android 已关闭的卫星/路网装配、imagery URL override 死合同，以及 iOS/macOS 默认 Gaode XYZ raster 路径均已物理删除；生产仅剩官方纯矢量路径。通用 MVT 几何仍保留 `VectorRasterStyle`/`MvtTileFetchCache`，不构成 raster 双路径） |
| I01a | official scene 只组合官方矢量与 Scene terrain | PASS（Facade 拒绝 generic MVT/raster/custom overlay、任意 glTF、offscreen passthrough、FXAA、aerial fog、三种 VT/composite PoC 与 TerrainPageStore，但允许唯一 Scene terrain 为 ellipsoid 或 Heightmap；Engine 的三个公开 PoC setter 与 official runtime 也双向互斥：先开 PoC 会拒绝 official，official 已存在则 setter 返回 false。provider 统一经 official land decorator，只改变 terrain primitive 的官网底色/unlit 表现，不改变高度、瓦片拓扑、availability、revision、请求诊断或官方矢量身份。旧 fixed flat canvas factory 和 Heightmap 拒绝合同已物理删除。重复 `installScene` 原子替换同一个 primary terrain，不再出现 config 已提交但旧 provider 仍渲染；测试通过 replacement URL 的实际请求证明 rendered provider 与 config 同步。public Tileset 入口仍不可旁路替换，SDK 仅经 private friend 安装/替换该唯一无 raster terrain Tileset） |
| I01b | 当前候选帧 emitted terrain surface 驱动全部 official 矢量派生物重贴地 | PASS（官方矢量 Scene 接线已从 registry 最深常驻 DEM 的 `TerrainHeightService::RenderGridConsistent` 单路径迁移到 `RenderedTerrainSurfaceSampler`。命令以实际 selected/render tile identity + selected/render key + pass 的完整同帧身份索引匹配 composition entry，不会被 additional tileset 同键命令冒充，也不会合并共享 tile pointer 的不同 composition fragment；匹配从每 entry 全 command 扫描收敛为一次建索引 + 每 entry 一次查找。最终 grid/morph/fade、RG16 高度纹理量化、ancestor remap 的最终扩张 `clipUv`、GLES edge snap/LUT 及 LUT 16-bit 差值量化均进入 CPU 采样；格内高度已从错误的四节点双线性 patch 改为 GPU 模板真实 `a-c-b / b-c-d` 两三角形分段线性面。影像 clip 与 DEM 高度 remap 不再混淆投影语义：只有 Geographic clip 可进入 mode 2；WebMercator clip 明确保持 mode 1 的祖先真实几何 + 投影 discard，避免把投影 V 当成地理纬度 UV 采错高度。LUT 批量 flush 已提前到 Feature 重贴地前，任一上传失败则候选帧 GPU/CPU 同时清 valid 位并退回自纹理吸附。edge snap 只属于真实 displacement shader：baked/legacy glTF 地形在最终命令盖章时强制清零，sampler 也以 `hasTerrainDisplacementFrame` 二次 fail-closed，不再让 CPU 模拟 GPU 根本不存在的吸附分支。Metal 当前不具备同一 snap/remap shader，命令构建会显式关闭这两种 GPU 行为，因此 sampler 不再伪执行仅 GLES 存在的分支。数值测试覆盖 direct、ancestor、composition、command miss、跨 tileset 同键、共享 pointer fragment、Geographic/WebMercator clip 分流、remap、RG16、三角拓扑、LUT、无 LUT fallback 与 baked shader capability；生产 `Scene.render.buildBreakdown` 已记录 sampler build ms、candidate/index/lookup 数、LUT copy bytes、area build/candidate copy 数。真实 Scene A/B/C 回归进一步锁定整帧原子性：A 提交地形与旧矢量，B 已构建新矢量但 post-build hold 时既不调用 `beforeSubmit` 也不 submit，设备和 presentation trace 均保持 A，C 地形恢复后才将地形与新矢量共同提交；因此无需、也不存在跨帧 presented-sampler 双路径） |
| I02 | `devicePixelRatio` 进入 style pixel ratio | PASS |
| I03 | 不因对齐增加请求/refetch | PASS（Release emulator 真数据流：三个 official source 各 `desired=104/render=104`，Regions/Main 继续共享唯一 type1 cache。icon atlas taxonomy 只由官方生成记录导出，不维护本地 atlas 白名单；请求由可见官方 identity 精确 demand，同一 atlas 每代只请求一次。tile version 与 icon manifest 共享 runtime transport 的唯一 `/web/init`，旧 assets 独立 init 路径已物理删除） |
| I04 | 不增加不必要 geometry/upload/draw | PASS（Main/Regions/Poi 的 production 与测试 adapter 现共用同一 typed official converter；surface/building、transport、road-label、POI 均直接查询唯一内部 identity 合同，并在 Feature/property/ring 复制、坐标转换、winding normalize/CDT 之前丢弃未知 tuple；公开 label 头中的平行 POI identity 声明已移除。五类官方生成表均不再逐项线性扫描：POI label 2215 条、POI icon/dynamic-background 2320 条、transport identity 429 条、road-label identity 285 条、surface 354 条与 building 25 条分别由 compile-time `static_assert` 锁定生成顺序，并以 `lower_bound` 查找；同 identity 内仍按原 zoom-record 顺序选择，因此纯整数准入/解析由 O(N) 收敛为 O(log N + 同 identity records)，不改变任意浮点运算、颜色、宽度、icon、窗口、几何或输出顺序。worker 还只解析当前 geometry 实际消费的 identity：LineString 不再遍历 surface taxonomy，Point 不再遍历 fill/line taxonomy，无 outline 的 Polygon 不再遍历 transport taxonomy；每条普通道路确定性减少一次无输出的 surface expression，每个无描边面减少一次无输出的 line expression，表达式纯函数语义、几何/FP/分组与输出顺序保持不变。未知 identity 和缺失/畸形官方 zoom window 在 tessellation/upload 前 fail-closed；Road selector 仅准入至少一个官方 drawable pass，`20016:17` 等 width-only identity 不再 tessellate，visibility 只由 drawable record 计算；command-time lineType 表达式缺失、非数值或 NaN/Inf 时 fail-closed；表达式为数值但枚举未知时，唯一 resolver 必须重放官方 runtime 的 `default: "solid"` + butt cap，而不是本地近似。官方 casing 缺 styleGroup width record 时不再读取 generic ratio/extra-width，而是直接不发 casing；sealed profile 的公开 commit 边界会重验 fill/line/extrusion/symbol identity 表并拒绝 generic cross-profile mesh、raw stencil volume 与错类型 payload，不能再绕过 worker 准入上传 generic Point/Fill；sealed Main/Regions 的 store 路径也完全禁止 generic name-label 副产物。RegionLayer `20014:2` 既不生成/上传零宽线，也不借用 road-name label 合同；POI 不再通用解码后删除重解；已删除固定 z12 water 第二 source 的重复选择/镶嵌；同 drawOrder 不同 styleGroup/feature zoom window 共享 buffer、按 range 提交；sealed official fill/line worker 不再求值或烘焙 generic color/property-table，CPU 顶点仅携带中性占位与官方 styleGroup，official point/road-label worker 同样不再携带 generic size/offset fallback；污染测试锁定 generic 字段不能进入官方 payload。official Clamp stencil 因准入条件要求 `!officialFill/!officialLine` 对 sealed profile 不可达，保留的是通用引擎能力而非官方双路径。region polygon constraint intersection 现用 `2*kQuantum` 膨胀 AABB broad-phase，只跳过数学上不可能相交的边对并保持原 `(i,j)` 精确求交顺序；典型瓦 5,469,373 对中仅 5,219 对进入精确计算。CDT 在点插入完成后建立精确无向 edge-count index，constraint cavity 删除/重三角化时同步增减；`edgeExists` 不再为每条约束扫描全部三角形，同场景慢样本计数 `327,942,068→69,447`，positions/indices 与 diagnostics on/off 均保持一致） |
| I05 | FrameGate 稳态 idle | PASS |
| I06 | 无 FATAL/ANR/GL_INVALID | PASS |
| I07 | Release 构建、安装、前台运行 | PASS（`emulator-5554` 前台进程中 `AmapE3: atomic official runtime installed`；Regions/Main/Poi 均出现非零 desired/render/active 与 `VectorTessSlow`，type1 fetch=116；`rasUp=0`，无 FATAL/GL_INVALID） |
| I07a | manifest 协议构造仅由 runtime 私有 transport 使用 | PASS（公开 `AmapTileManifest.h` 只保留 data zoom 档位；endpoint/version/dataSource/request/url 结构与 build/parse/select helper 已下沉 internal header，API guard 锁定生产调用方不能构造 AMap-like 变体） |
| I08 | 视口/相机/DPR 固定的数值 A/B harness | PASS（固定官方 style PBF SHA `e7e7226e...ca`、runtime SHA `4b41a499...38` 与 WebGLRender SHA `2d2dd711...f5`；extractor 直接从 PBF 解析 `20001:1` provider z14 为 center 8、signed border 2，并由本地明确的 display→provider `+1` 合同得到 display z13；runtime/WebGL 唯一 consumer 链证明 face 8 / casing 10 CSS px 经 resolution expansion，官方 `retina?2:1` 仅允许 1×/2× 两分支，因此 DPR2 为 16/20 physical px、`1280×720` backing 为 `2560×1440`。宽度与 retina scale 均无 CLI 注入入口；缺失/重复 runtime needle 或缺失/重复 PBF record 均 fail-loud） |
| I09 | FPS、frame time、memory、IO、upload 基线 | PASS（最新 Release PID `23065` 在固定视口最终达到 Regions/Main/Poi 各 `desired=104 render=104 request=0 pending=0 tess=0 ready=0 active=104 pairs=0`，证明此前长期 `tess=8` 是单 worker 的有限 backlog，不是 drop/retry/generation 循环；type1 fetch 稳定为 118，refetch 14 后不再增长。最终 FrameGate 在无输入时稳定 idle；`memImageryKB=0`、raster upload/fallback=0，无 FATAL/ANR/GL_INVALID。加载阶段剩余主要成本仍是官方 `30001:2` surface polygon 的 Bowyer-Watson point insertion；保留完整细节与现有 double predicate 语义，不以降 LOD/裁剪内容掩盖成本） |

### I09 最新 Release 证据

跨 geometry 死 identity 求值移除后的新 APK/PID `22558` 已在
`emulator-5554` 前台运行；日志确认
`RuntimeAB amapVector=official-only aerialFog=0`、`rasUp=0`，且无
FATAL/GL_INVALID。main 的 `400–1975 features` 样本中
`admit=0.30–1.48ms`、`line=0.26–25.33ms`；对比旧进程最高
`admit=344.25ms/5052 features`，道路 taxonomy 准入已不再是主瓶颈。
当前重成本明确转移到官方 surface polygon/CDT；结构相同的
`30001:2/6628 points` 仍执行 `48,878,691` 次现有 double predicate，
下一轮必须继续直接保守于该 predicate，不能以数学近似或降低可见细节规避。
Bowyer-Watson cavity 的 directed-edge membership 已从每个输入点重新构造
`unordered_set` 改为复用连续 `uint64_t` scratch：边仍按原
`badTriangles × (v0,v1),(v1,v2),(v2,v0)` 顺序写入，只对查询副本排序并
`binary_search` reverse edge，最终发射仍严格遍历原 bad-triangle/edge 顺序。
因此 predicate、次数、顺序、浮点计算、boundary 判断和 indices 不变；按上述
重瓦 `59,870/10,069≈5.9` bad triangles/point，典型每点约 18 个 key 的连续
排序替代约 18 个独立哈希节点的分配、构造和 clear 销毁。CDT 9/9、Polygon
13/13、FeatureRenderLayer 176/176 通过。
新 Release/PID `22757` 在同一 `emulator-5554` 命中完全相同的官方重瓦
`z10/817/341`：`polyInput=10069`、`cdtPointTests=48,878,691`、
`cdtBad=59,870`、`cdtEdgeLookups=10,104`、
`cdtCrossTests=2,895,181`、constraints `9,829/275`、peak tris `13,037`
均逐项一致，确认 rewrite 未改变 cavity 或后续几何状态。point insertion
`176.38ms→175.14ms` 仅约 `-0.7%`，落在设备调度/DVFS 噪声内；不把它写成
速度收益，只认定确定性的逐点哈希节点生命周期消除。

## 当前执行顺序

I04 完成审计：**PASS**。官方生成表可从固定 PBF/runtime 逐字节重放；
API/symbol guard 证明 production 只有 sealed runtime 路径；decoder/source
测试证明 unknown 与零输出 payload 在几何分配前 fail-closed；FeatureRenderLayer
回归证明 generic payload、range-less mesh 与跨 profile payload 无法进入 official
upload/draw；当前 Release 证明 `rasUp=0` 且 main admission 已收敛。上表 I04
长行保留完整历史说明，本段是其最新状态结论。

1. 继续按 A–I 合同做 completion audit；CDT 后续只接受能保持现有 double
   predicate 集合、次数、顺序和输出 indices 的严格等价改写。

每完成一项，必须把状态从 `FAIL/PARTIAL/UNKNOWN` 改为 `PASS`，并在同一行
补齐测试或证据位置。未经合同矩阵记录的视觉改动不进入实现。
