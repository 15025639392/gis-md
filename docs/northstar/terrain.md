# 地形模块北极星 — 产品体验判据

**这份文档回答「做到什么程度算好、现在到哪了」。**
不回答「代码在哪」(那是 `AI_INDEX.md`),也不回答「当时怎么修的」(那是 `docs/issues/*`)。
`docs/issues/` 是写完即冻结的历史档案;**本文是活的**,随每次专项收官更新。

用法、状态符号、【机制】/【观感】分工、「代价」列硬要求、`推断` 标记的含义
一律同 `vector.md` 的「怎么用」节,不在此重复。

## 编号命名空间(与 vector.md 的约定)

`vector.md` 已占用 `V1`–`V24` / `P1`–`P7`。为免「V3」跨模块歧义,
**本文判据一律带 `T-` 前缀**(`T-V1` / `T-P1`),vector 既有编号不动。
未来 `imagery.md` 照此用 `I-`。

> ⚠️ **本方案尚未经你确认。** 目前没有任何历史对话引用过 `T-*` 编号,
> 此刻更换命名方案成本为零;一旦开始引用即不可逆(编号只增不改)。

---

## 本文档的诚实边界

**下列判据绝大多数带 `推断` 标记 —— 它们是我从代码、`docs/issues/*` 与既往会话
记录反推的,不是你直接对我提过的地形体验要求。** 按 `vector.md` 的元规则,
推断判据「校错了比没写更糟」,请优先校对这一批。

唯一有你直接背书出处的是 T-V1/T-V2/T-V3 的触发点:
`terrain-visual-maturity-gap-2026-08-02.md` 开篇记录你的原话反馈
「瓦片/地形渲染板块不像一个成熟的地球引擎」,澄清后归为
**① 几何/地形质感差、② 加载期不体面**两类。本文的 A/B 两节即由此展开。

---

## A. 几何质感(「不像成熟引擎」的第一类)

| # | 判据 | 类型 | 状态 | 证据 | 代价 |
|---|---|---|---|---|---|
| **T-V1** | 地形三角形不应在默认视角下横跨数十像素 | 【观感】 | ❌ `推断` | 整条高度链钉死 65×65 → **133 m/三角形,默认相机下横跨 ~46 px**(1080 屏)/~100 px(2400 屏)。参照系 Cesium World Terrain 自适应 TIN 有效 ~10–30 m,**我们粗一个数量级**(gap 文档 §1.1–1.3) | 抬到 256 可拿回 16.6 m,代价顶点数 16×、高度纹理层 16× 显存 —— **未实测,需取舍** |
| **T-V2** | 坡面不出现逐三角形亮度台阶(刻面) | 【观感】 | ⚠️ `推断` | 法线与几何解耦**已落地**,机制自证通过(探针 93.47% 屏幕像素走新分支)。但画面几乎没变 —— 收益被 T-V3 闸住(gap 文档 §4.4) | 一张法线贴图 + 槽 23;**未量化** |
| **T-V3** | 地形有可读的明暗起伏(relief 不被光照压平) | 【观感】 | ✅ 代码已落地,观感 A/B 待你拍板 | 修复已落地(2026-08-12,`1a939be70`):四个地形片元 shader 收敛为单一治理点 `TerrainSurfaceLightGLSL.h`,弃用 smoothstep、改线性 Lambert(`clamp(NdotL*0.9+0.3)`,Cesium/osgEarth 同款),GLSL/MSL 双写同步;header 注释直接引用本节 0.992 测量为动机。**剩余**:①真机 A/B 拍板线性曲线的实际观感(可临时切回 smoothstep 版对比);②MSL 两变体 sunTint 仍为内部常量未参数化(B4,随 Metal 端口统一) | 0(GPU 零增量) |

| **T-V9** | 运动中地表不得出现黑格/黑带(瓦片画不满自己的地理范围) | 【机制】 | ⚠️ 修复已落地,**验证加强**(2026-08-22):9480 帧(含平移 + 2 轮全局↔33km 脚本化缩放,跨度覆盖 z6-12 全部行)`spanMis=0` 持续、肉眼无黑带。**转 ✅ 仍差你手捏合一轮**(脚本化缩放≠捏合,粘滞现象的证据等级按原判据收口) | **根因=共享位移模板按 `{schemeId,z,row,gridSize}` 缓存,由第一个来要的瓦片 bounds 定型且永久不自愈**;跨度不同的瓦片落到同一键 → 后来者拿到半宽模板,几何只铺一半、四周露背景。真机取证:`spanMis>0` 的 43 帧 dark 均值 0.0735,`spanMis=0` 的 8 帧 0.0003(**差 245 倍**);经度倍率恒为精确 2.000 = `rootTilesX` 差一倍的两套切片方案。修法=把地理跨度并进缓存键 | 0(键多两个混合项;真有多套跨度时多存几份模板) |

| **T-V10** | 瓦片边界不出现发白细线(**竖线**;横线从未观测到,见下) | 【观感】 | ✅ 达成(2026-08-16) —— 修 `f4a440ea9` 后**你真机同机位判定竖线消失**;host 守卫双向验证 82.28°→<3° | 根因①见下方「T-V10 交接」。⚠️ 真机跑的是 GPU 烘焙那份,而 host 守卫只咬得住 CPU 孪生实现,GPU 那份的证据**仅此一次肉眼判定** | 0(烘焙内多几处有效性分支) |

**T-V1/T-V2/T-V3 的依赖顺序**(gap 文档 §三的排序,已按"最干净"排):
T-V3 **必须先于** T-V1 —— 不解决光照压平,几何抬到 16.6 m 也照不出来。
**2026-08-12 起该前置已解除**:T-V3 的代码改动已落地(线性 Lambert,见上行),
高太阳角下 relief 不再被 smoothstep 饱和压掉;T-V1/T-V2 的收益现只受
几何密度与法线细节本身限制,不再被光照闸住。

无阴影、无 AO(`ssao`/`ambientOcclusion` 全库零命中)。gap 文档判定:
几何还是 133 m 时加阴影意义不大,列第二梯队。**本文不为其立判据**,
待 T-V1 有进展后再议。

---

### T-V10 交接(2026-08-16 收集,下次冷启动可直接接手)

**现象**:瓦片边界出现**发白**细线,竖横都有、构成网格。GPU TERR 置 OFF 即消失。

**已钉死的事实**(都可复查,别再重测):

| 事实 | 依据 |
|---|---|
| 线是**发白**,不是背景色 | 放大图为奶油/灰白窄带,两侧地形连续 |
| **逐像素对比度仅 1~7 亮度单位** | 阈值 12 的逐像素检测器对肉眼清晰的线返回空 |
| 宽度量级 = **一个网格单元**(z7 约 4.9km≈7px) | 亚像素解释被排除:1km 台阶在 4° 偏天顶下只投影 0.1px,裙墙同理 |
| **不随高度单调** | 1.69Mm 有(6.1σ)、198km 有(9.8σ),而 435km/839km 无,11~75km 无 → 跟随屏上恰好出现的某类瓦界,不是"高空才有" |
| 需要 GPU 位移 ON | 真机开关 A/B |

**最像的方向**(未验证):发白 + 极低对比 + 一个单元宽,三条合起来最像**边界法线不连续
导致的着色差**——几何缺口会露背景色(见 T-V9 的黑带),贴图错位会有内容错位而非均匀发白。
相关既有资产:`test_terrain_edge_normal_seam`、`bakeTerrainHeightNormalTexels`
(边缘法线需要邻居数据)。**注意**:那个 host 测试测的是烘焙函数,真机上还叠了
relief fade、边高度 LUT、以及"邻居可能是不同 z",三者都不在它覆盖内。

**根因①:源重叠环整列 no-data + 烘焙拿海平面 0m 去求斜率**(2026-08-16 定位并修)

| 事实 | 依据 |
|---|---|
| 生产 NASA 514 源**普遍缺西/北重叠环**:抽样 19 片,14 片的 px 列 0 或行 0 整列 `code=0`;东/南环完好 | 直接拉瓦片数比:`7/102/53` 西环 514/514 全零,而 `7/101/53.col513 == 7/102/53.col1 == 908.4m`(环语义本身是对的) |
| 该缺陷形态与观感形态同构 | 西环缺 → 竖线,北环缺 → 横线,**逐瓦片零散**——对上「网格状」「只在某些瓦界」「不随高度单调」 |
| 烘焙把 no-data 当 0m 送进边界差分,而边界臂只有 `reach×跨度`(z7 ≈266m) | 900m 地表对 0m 的假落差 → 边界节点法线被打到近水平。测试场实测**夹角 82.28°**(修后 <3°),模型算真实瓦片 `nz 0.999→0.05` |
| **GPU 烘焙还多错一层**:`sampleH` 没有移植 CPU 的「no-data 角剔除后重归一化」,把 0m 混进双线性 → 瓦片西边界**高度直接砍半**(908→454m,与邻瓦差 441m) | 两份代码对读;GPU 烘焙是 GLES 默认路径 = 真机跑的就是这条 |
| 观感是**光照项变亮**,不是贴图漏白 | 复现图 x=363 处增量通道比 ΔR/R=6.0%、ΔG/G=6.6%、ΔB/B=4.7%;白色叠加会让最暗的 B **相对**抬最多,实测相反 → 排除「影像/页存储漏底色」一族 |

修法:no-data 邻居 → **丢掉那条臂**(退化为另一侧单边差分),no-data 中心 → 法线取平;
GPU 侧同时补上 no-data 角剔除重归一化。CPU/GPU 两份逐条对应,守卫
`TerrainEdgeNormalSeamTest.NoDataOverlapRingDoesNotWreckEdgeNormals`(用真函数,
非复刻)。⚠️ **GLSL 那份 host 上没有执行级守卫**(无 GL 上下文),只做了语法编译校验;
真机之前它等同未验证。

**验收(2026-08-16)**:真机同机位 A/B —— **竖线消失**(你判定)。据此转 ✅。

⚠️ 收官时仍未结的三条(不影响 ✅,但别当它们已解决):
1. **「横线」这条判据从来没有证据**。唯一复现资产 `vline_repro_1p69Mm.png` 上
   `seam_line_detect.py --horizontal` 返回空,而同图竖线仍报 6.1σ(阳性对照有效);
   你也从未看见过横线。机制上北环缺失与西环缺失同源,理论上该有 —— 一个未验的猜测
   是太阳方位偏东西向时南北向法线偏转对 NdotL 的调制小得多。注意横向那趟只有 290 行
   样本 /σ=1.58,灵敏度低于竖向(647 行 /σ=1.10):这是「这张图上没测到」不是「不存在」。
2. **量级落差未解释**:修前边界法线错 82°,屏上却只有 6 个亮度单位的对比。合理解释是
   高空大气/雾把地表对比整体压掉,**未证**。
3. **GPU 那份改动没有执行级守卫**:host 无 GL 上下文,只做了语法编译校验;它的正确性
   证据就是这一次肉眼判定。CPU 孪生实现的守卫替不了它。**已立为 T-P6**。

**工具**:`tools/seam_line_detect.py` —— 沿线方向平均的检测器(逐像素阈值必然失败)。
已标定:`docs/assets/tv10/vline_repro_1p69Mm.png` → x=363,超出 6.68,**6.1σ**。
⚠️ 改动脚本后必须重跑这个阳性对照,否则"测不到"与"没有线"无法区分。

**复现**:相机 camH≈1.7Mm、近正俯视,或 camH≈200km;`geoZ` 跨 0-7 / 0-10 时命中率高。


## B. 加载期体面(「不像成熟引擎」的第二类)

| # | 判据 | 类型 | 状态 | 证据 | 代价 |
|---|---|---|---|---|---|
| **T-V4** | 交互期(拖动/缩放)地形上传不硬冻结 | 【机制】 | ✅ | `294bff2ed` 删 Urgent-only 早退改 budget lane 涓流。A/B:**积压 48→0,暂态 0.53s→0**(gap 文档 §2.1/§2.2) | 与影像侧 `uploadAllowedDuringInteraction` 同构 |
| **T-V5** | 稳态无漏天/漏底 | 【机制】 | ✅ | 无缝专项收官(边高度 LUT + 档位单一决策点);稳态漏天**精确 0**。验收判据是 appRate 而非"不崩" | — |
| **T-V6** | 加载窗口不露"海平面凹坑" | 【观感】 | ⚠️ `推断` | fill 代理改借最近已加载祖先高度 + 加裙墙(`89d57ac2b`),代替原先停在 h=0 的平代理。真机 demo 已开启(`MinimalGlobeDemoConfig.cpp:189`) | 每次首见帧/祖先换页帧 17²=289 次 O(1) 双线性采样,**同步、无帧预算封顶**(见 T-P3) |

---

## C. 源覆盖与兜底

| # | 判据 | 类型 | 状态 | 证据 | 代价 |
|---|---|---|---|---|---|
| **T-V7** | 源未覆盖区回落平滑椭球,而非粗叶子 + 巨型裙墙 | 【机制】+【观感】 | ⚠️ `推断` | **机制已通**:`082c75250` 让 zoom 下界进 `availabilityState`,`CompositeTerrainProvider` 的椭球兜底重新可达(此前 z0-5 恒报 Available,兜底整段短路)。单测钉死正反两侧,host ctest 187/187。**观感未验** —— z0-5 从"发 404 请求"改为"走椭球"后的实际画面、以及与 z6 交界处高度悬崖,**待真机截图,归你判定** | 未量化(减少了 z0-5 的无效 HTTP 请求,未计数) |

**设计契约**(`no-fine-data-ellipsoid-fallback-design-2026-07-07.md` Approach A §77):
primary 在未覆盖处**必须**报 `NotAvailable`,Composite 才能路由到椭球。
该文档写于 ion 全球源时代,明说此机制「只在真·部分覆盖数据集下出现」;
`a40a33304` 退役 QM/ion 换成全球 514 源(z6–12)后,**覆盖语义从空间维度变成
zoom 维度**,判定却留在旧维度 —— 这是 T-V7 此前失效的成因,不是"谁写漏一行"。

**边界(设计文档 §4.4,有意不做)**:不隐藏覆盖区与椭球区之间的**真实高度悬崖**
(如高原 +4km 接海平面 0)。要消的是 385km 裙墙与粗台阶,不是这个诚实落差。

---

## D. 跨平台

| # | 判据 | 类型 | 状态 | 证据 | 代价 |
|---|---|---|---|---|---|
| **T-V8** | Metal 后端地形能力与 GLES 对等 | 【机制】 | ❌ `推断` | 三处未接线,**均有显式检测 + 安全回退,是"未做"不是"做错"**:①深度 prepass `TerrainDepthPrepass.cpp:15` 无 shader 即 `return false`;②实例化 MSL 在 `Renderer.cpp:4143` 被 `(void)` 掉(注释:留合批 Step 4);③GPU 烘焙 `TerrainDisplacementTemplatePool.cpp:323` 仅 OpenGLES,非 GLES 回退 CPU | 见 T-P1 / T-P2 |

⚠️ **接线时的已知陷阱**(散在源码注释里,汇总于此免得再靠 grep 重新发现):
- 实例化 MSL 片元源须走 `withTerrainLight(kTerrainInstancedFragmentMSL, metal=true)`,
  该字面量已改为调用 `terrainSurfaceLight`,不注入函数定义则编译不过。
- `kTerrainInstanced{Vertex,Fragment}MSL` 自称是 GLSL 实例化对的镜像,但**从不编译**;
  GLSL 侧已加法线贴图分支而 MSL 侧没同步,接线时须一并补。
- bake shader 只有 GLSL(`kTerrainBakeFragGLSL`)。Metal/Vulkan 强走 GPU 路径会因
  `createShader` 失败使高度层**永不烘 → 地形变平**,故硬回退 CPU 而非静默失败。

---

## E. 性能债(明记,不假装没有)

| # | 债 | 量化状态 |
|---|---|---|
| **T-P1** | Metal 从未绑定地形高度纹理:`RenderDeviceMetal.mm:1114` 绑定循环上界曾 = 22,而 `kGltfHeightTextureSlot = 22` 被排除 → Metal 侧 GPU 位移形同休眠。上界已随槽 23 扩容改为 `kGltfTerrainNormalTextureSlot + 1`,但**位移本身仍未接线** | 与 T-V8 同根 |
| **T-P2** | 非 GLES 后端回退 CPU 烘焙高度层 | ✅ 已量化 CPU 侧(2026-08-22 host,Release -O2 arm64):`bakeTerrainHeightNormalTexels` 每瓦 coarse(65²)=0.087ms、dense(257²)=0.938ms;GPU 路径 CPU 侧打包(514² 源)仅 0.130ms → CPU 烘焙多付 ~0.81ms/瓦(dense)。20 瓦 dense 一帧 ≈ 18.8ms(host,手机预计 2-5×)。**不单独立项**:修复归属 T-V8/T-P1(Metal 补 MSL 走 GPU 烘焙);若 iOS 真机 churn 尖刺复现,CPU 烘焙是候选元凶,先分解再决定。GPU RTT 本体 host 不可测(T-P6),成本差仅 CPU 侧钉死。证据:`test_terrain_cpu_bake_cost`(Debug 19.9ms/瓦是 -O0 放大 ~21×,勿用) |
| **T-P3** | fill 代理构建同步且无帧预算封顶:`TileUpdateSelectionWorkRunner.h:237` 对每个可见瓦片循环调用 `ensureFillProxy`,`fillStartMs` 仅事后计时,无 break/budget | ✅ 已量化(2026-08-22 host,Release -O2 arm64):每瓦全量构建 mean 0.072ms(median 0.054,grid16;grid8 0.017/grid32 0.218,~grid² 缩放);burst 32/64/128 瓦一帧 = 3.8/5.0/10.6ms;稳态签名早退 0.00003ms;高度采样增量 0.006ms/瓦。**不立项**:单瓦成本已足够低,帧预算收益有限;该循环里更贵的影像 prefetch 半程已有 `frameResourceBudget` 节流。若真机 churn 尖刺仍现,先分解 `prefetchFill`(fill 构建 vs 影像映射),勿先封构建。证据:`test_fill_proxy_build_cost` 4 测试(Debug 1.09ms/瓦是 -O0 放大 ~15× 的假象,勿用) |
| **T-P5** | 谁和谁撞了同一个模板键**未定位**:`std::hash<SchemeId>` 哈希的是 interned 指针,本该区分两套 scheme —— 所以要么两者被 intern 成同一 handle,要么某调用点用 A 的 key 配了 B 的 bounds。T-V9 的修法是结构性兜底(键含跨度),**没修元凶** | ✅ 已定位=**结构上不可达**+看门日志已加(2026-08-22 真机+静态):① `SchemeId::intern` 按字符串判等,不同 scheme 字符串必不同 handle(64 位哈希碰撞可忽略);② 两处 `acquire()` 调用点 key/bounds 同源一致(无 A key 配 B bounds);③ 唯一残余理论风险=同 scheme 字符串不同几何(OpenGlobus 三分区),当前 app 单 scheme(XYZ-WebMercator)不可达。新增 `TEMPLATE_BUILD`/`TEMPLATE_SCHEME_MISMATCH` 节流日志(请求方 vs 建模方 schemeId 字符串,命中比对 O(1) 指针)。真机 9480 帧(平移+2 轮全局↔33km 缩放):`spanMis=0`、0 次 scheme mismatch |
| **T-P4** | HDR 变体常数是 provisional:`TerrainSurfaceLightGLSL.h:52` 的 `shadowFloor=0.15`/`ambientScale=0.6` 明标未定,真正调参在 T2 对着 tonemap 输出做 | 有主(T2),flag 默认关 |
| **T-P6**<br>(验证债,非性能) | **shader 在 host 上没有执行级守卫**:测试进程里没有任何真 GL 设备,GLSL 只能靠肉眼在真机上验。后果已兑现一次 —— GPU 烘焙 `sampleH` 漏移植 CPU 的 no-data 角剔除,两份实现静默分叉很久无人发现(T-V10 根因①)。**注意这不是"CPU 那份多余"**:GPU 烘焙是帧内路径(`SceneRenderPipeline.cpp:236` 每帧 flush,靠 RTT + `setFramebufferColorLayer`),没有 device/没有帧的场合根本不可达,删掉 CPU 等于把仅有的可执行实现也删掉 | **方案 B 已落地(2026-08-24)**:新增 `test_glsl_compile`(`tools/check_glsl_compile.py` + `tools/fetch_glslang.sh`),把主要 GLSL ES 源(含 createShaders 注入后的最终形态:terrain/gltf/instanced/vector/point/label/bake/SkyBox/FXAA)离线编译,**HDR 冻结态注入变体同批编译**(L-P3 首次被自动化触碰);45 PASS / 41 显式 SKIP。**仍未覆盖**:①大气/tonemap/aerial fog(依赖 C++ 生成函数拼装,Python 复制会引入第二事实源)②MSL(需 Metal 编译器)③数值级 CPU↔GPU 逐 texel 对拍 —— ①②③归方案 A(host 离屏 GL),留触发制。其余:**未量化**。解法排序:①host 离屏 GL(EGL/OSMesa 无窗口 context)跑真实 bake pass 与 CPU 版逐 texel 对拍 —— 一次性投入,之后所有 shader 改动都受益,**我押这个**;②黄金向量对拍(CPU 存 golden,真机诊断开关跑 GPU bake 比对)—— 便宜但要真机在手,进不了 ctest。<br>"只留 GPU 一条路"的前置条件是三件事同时成立:MSL/SPIR-V 补齐 + 真 Metal 设备验过(均属 T-V8 欠账)+ 本条解决 |
| **T-P7**<br>(候选,发热线程 2026-08-18 交;矢量 P9 转此) | **高度纹理烘焙同步现烘,churn 期尖刺**:`TerrainDisplacementTemplatePool::acquireHeightTexture` 缓存未命中即现烘现传;**dense 档有 `denseBudget` 逐帧限流,coarse 档无**。平移带新瓦入视触发 → `rebuildCachedDrawCommands` 内 `rebuild=` 尖刺。属 **T-P3 同类**(sync、弱/无帧预算、churn 尖刺),机制不同(高度层烘焙 vs fill 代理) | **部分量化**:release -O2 debuggable 真机(2026-08-18)测到单次 `rebuild=16ms rebuilds=1`/瓦。⚠️⚠️**caveat**:①debug -O0 曾把同路径放大到 `rebuild=129ms`(**假象,必 release 评估**);②直接分解慢帧 `engine` 证实成本在 **`build=` 阶段**(`upd=` 瓦片选择仅 0.5ms、`submit`/`terrUpd` 各 ~0.5ms、`vector=` ~1.3ms、`swap` ~1ms)——本烘焙(`rebuild=`)是 build 里**唯一可复现的尖刺**;③更大的间歇 `build=40-60ms` 尖刺 run 间不一致、buildBreakdown 抓不到对应分量 = 噪声,logcat wall-clock 分不清计算 vs 渲染线程小核调度(`cpu=` 混杂 4-7,见 vector 侧 ADPF 老问题),须 simpleperf 才能归因。**低优先、measure-driven**:静态发热已由层1(引擎侧太阳角门控,`d53b53718`)根治,运动 borderline 非缺陷级。修向:coarse 档也加逐帧烘焙预算(仿 dense `denseBudget` / vector P6 字形 4ms)。**先决**:该不该修取决于 simpleperf 归因(可复现 9-16ms 本条 vs 间歇 40-60ms 噪声/调度)——**曾误判在 Tileset 选择,已 `upd=0.5ms` 直接证伪** |

---

## F. 已判死 / 勿再提(边界)

- **逐瓦片高度量化**:破坏无缝所需的逐位相等。正解是**全局固定格点**,不要再提逐瓦片方案。
- **隐式瓦片(implicit tiling)**:零引用,但**你已裁决保留**,不要再提议删。
- **地形 draping 走影像路径**(矢量 E4-4):根因已重定位,正解是矢量进页存储,不是继续修 drape。
- **V1818T(Adreno512/720p)GPU 帧率优化**(2026-08-19 专会,4 build ablation 真机 + 同日等价重写专会补正):churn 修复后 GPU ~140ms/帧。**ALU/pass 层四杠杆全死**:①天空散射→LUT(compute 实测 0)②雾折进终端(拆 pass 净 0 反更慢)③场空邻域哨兵(gather 早被 own-check 门住)④满驻留丢 mappedRaster(合批已 count=0)。MSAA 4x/2x/1x 全同帧时(TBDR 片上 resolve 免费,**勿关**)。**补正:第四层 = texop 计数**——校准单次全屏 texelFetch≈3.3ms/bilinear≈6.3ms,terrain 是 texop-bound;**T1 已收**(`a744801c2`,法线场 4 fetch→1 硬件双线性):terrain 86→77ms、总帧 140→**131ms**;**T2(两张间接纹理合 RGBA16UI)真机证伪勿重试**——功能正确但 +3.5ms(Adreno512 宽 texel fetch 半速)。**texop 层也已到底,现地板 131ms**;再往下只剩分辨率=观感。PHK110 50-60fps 正常档。详见记忆 [[gpu-ceiling-fill-bound-null-result-2026-08-19]]。

---

## G. 待你拍板的开放项

| 项 | 我的建议 | 影响判据 |
|---|---|---|
| **编号命名空间 `T-` 前缀** | 我押带前缀(vector 已占 V1–V24,无前缀必歧义)。**此刻零成本可改,一旦被引用即不可逆** | 全文 |
| **本文推断判据的校对** | 除 A 节触发点外全是 `推断`。请优先校 T-V1/T-V6/T-V7 —— 这三条我押的"什么算好"可能根本不是你要的 | T-V1 / T-V6 / T-V7 |
| **T-V3 光照动态范围** | **代码已落地**(2026-08-12,`1a939be70`,线性 Lambert 单一治理点 `TerrainSurfaceLightGLSL.h`),不再闸住 T-V1/T-V2。剩余 = **真机 A/B 拍板观感**(临时切回 smoothstep 版对比即可)+ MSL sunTint 参数化(B4) | T-V1 / T-V2 / T-V3 |
| **T-V7 真机验收** | 修复已落地但只验到单测层。要不要我打包 APK 推真机看 z0-5? | T-V7 |

---

## 更新协议

同 `vector.md`:专项收官改状态并附证据(commit / 真机数据 / 截图),
新增开销同一次提交更新「代价」列、测不了就进 E 节记债(**不许填"应该很小"**),
你提出新体验要求立刻加判据(哪怕状态是 ❌),方案被否决记进 F 节附死因。
**不做定期 review** —— 只在有事发生时动。
