# 相机 / 手势模块北极星 — 产品体验判据

**这份文档回答「做到什么程度算好、现在到哪了」。**
不回答「代码在哪」(那是 `AI_INDEX.md`),也不回答「当时怎么修的」(那是 `docs/issues/*`)。
本文是活的,随每次专项收官更新。

覆盖范围:自由地球相机(`FreeGlobeController`)的手势交互——单指拖拽(pan)/
双指缩放·旋转·俯仰(pinch/rotate/tilt)/惯性滑行(fling)/碰撞与俯仰约束。
**不含** 相机飞行编排(`CameraFlight`)、约束求解器内部数值(那是机制细节,
只在债里点)、瓦片调度对 `cameraMoving` 的消费(那在调度侧)。

## 怎么用

- **判据编号(C-V1…)是稳定引用锚点。** 你说「C-V5 我不满意」即指该条,不必重描述观感。
- **状态**:✅ 达成(有证据) ⚠️ 部分达成/有已知缺口 ❌ 未做/判据已立未测 🔒 待你拍板
- **类型决定谁说了算**:【机制】我自证(命令/计数/测试红绿/真机数字);【观感】像素判断**归你**。
- **债编号(C-P1…)** 同为引用锚点,见 C 节。
- **`推断` 标记**:带此标记的判据是我从现状推的,不是你说过的,优先请你校对。

**命名空间**:本文用 `C-` 前缀(`C-V*` 判据 / `C-P*` 债)。理由见项目 `CLAUDE.md`——
无前缀会让「V3」跨模块歧义。

**验证基建**:手势判据靠**真机闭环**验——免 root 双指注入台
([injector 模块](../../scaffold/examples/android/injector/))注入合成手势 →
引擎 `CAMPROBE` 逐帧吐 pose+anchor →
[`tools/cam_probe/camprobe.py`](../../scaffold/tools/cam_probe/camprobe.py) 算 anchorErr。
详见 `tools/cam_probe/README.md`。

---

## 北极星一句话

> 手指按住地表哪一点,那一点就**死死钉在指下**——pan/pinch/rotate 全程锚点不漂
> (亚像素);惯性松手顺滑收敛不跑飞;相机**不穿地、不锁死病态俯仰**;组合手势
> **各轴独立**不串味。手感(跟手、阻尼、灵敏度)自然到位。

**现状定位(2026-08-18)**:pin 保锚已**真机闭环坐实**——平地 drag anchorErr 0.06px、
增益 1.000(C-V1);近碰撞退化 regime 也已压测有界(C-V3)。短板不在"钉不住",
在**判据基建薄**:轴隔离/惯性收敛这些机制**代码里有、但没被 CAMPROBE 量化**
(C-V2/C-V4 挂 ❌),且手势全链**零自动化回归**(C-P1)——改相机代码只能手动真机验。

---

## A. 体验判据(C-V)

| # | 判据 | 类型 | 状态 | 代价 | 证据 / 差距 |
|---|---|---|---|---|---|
| **C-V1** | **pin 保锚**:手指下世界点跟手,pan/pinch/rotate 全程 anchorErr ≤ 1px | 机制 | ✅ | CAMPROBE 探针仅 debug 变体、每手势 START/MOVE/END 各一行 logcat,release 零开销 | **真机闭环坐实(`16531b80c`)**:平地 drag 43 帧末 0.01px / 峰 0.06px、**本该位移=实际位移=445.5px、增益 1.000**;pinch/rotate 峰 <0.05px。pin 解算求"让 anchor 投到 finger 的 pose",clampNow 沿 eye→anchor 线退出保锚 |
| **C-V2** | **轴隔离**:pinch 只动 range / rotate 只动 heading / tilt 只动 pitch,不串轴(契约 2.2) | 机制 | ❌ | — | **判据已立、未量**。代码机制在(InputManager 每轴激活标志独立门控),但没用 CAMPROBE 的 pose 反解逐手势验 Δheading/Δpitch/Δrange 的串扰。CAMPROBE 已吐 VP 矩阵,补一段 pose 反解即可测,见 **C-P2** |
| **C-V3** | **近碰撞保锚上界**:高俯仰贴地形时保锚退化有界,anchorErr ≤ ~0.5px | 机制 | ✅ | 无额外开销(径向 fallback 是既有兜底) | **压测坐实(`896edfc79`)**:俯冲触底稳定卡 eyeAlt≈350m(地形~300+`kMinClearanceMeters`50)。高俯仰贴底 anchorErr 峰 ~0.4px(基线 7×)= `constrainEye` 末行**径向抬升 fallback 发火**(eye→anchor 线近水平 `gain<kAnchorExitMinVerticalGain=0.2`)。**亚像素、自限**(远锚点下径向 Δh 换算像素角漂本就小;更极端即抓取 miss 退 spin)。**非 bug**,验证注释"该 regime 保锚判据本就不适用" |
| **C-V4** | **惯性收敛**:fling 松手后角速度单调衰减到 `kMinInertiaAngularVelocity` 后停,不跑飞/不早停 | 机制 | ❌ | — | **判据已立、未量**。真机联系表看到 fling 有惯性滑行(after→settle 又移一截),但没用探针抓 `inertiaAngularVelocity_` over tick 的衰减曲线。缺口同 C-P2 |
| **C-V5** | **tilt 灵敏度**:双指俯仰跟手、阻尼不过冲、灵敏度合适 | 观感 | 🔒 | — | **像素判断归你**。⚠️ 本轮观察「600px 拖动直接从俯视压到近掠视」疑**偏灵敏**,钉死场景待你拍板。**无几何 ground truth**:俯仰是"手指px→多少度 pitch"的灵敏度设计,非刚性锚约束,anchorErr 对它不适用 |
| **C-V6** | **不穿地 / 不锁死病态俯仰**:碰撞守卫顶住地面,俯仰约束拒绝恶化净空的方向 | 机制 | ✅ | 廉价地形预判(滤波高,不重采样) | 俯冲触底稳定卡 eyeAlt≈350m 不再下沉(`constrainEye` 楼层 = `max(filteredTerrainHeight,0)+kMinAltitude`);`rotateCameraVerticalAroundPoint` 净空守卫拒绝"让净空更差"的 tilt(贴底时 tilt 压不满俯仰即此守卫)。**零死区离合**:反向立即响应 |
| **C-V7** | **手势不崩**:双指手势不 crash、pose 无 NaN | 机制 | ✅ | — | 本轮全 6 手势注入无崩溃。⚠️ 排查记录:曾现"每次双指后 app 回桌面"**非引擎 crash**,是 instrumentation targetPackage 跑完 force-stop 的**测试框架伪影**(见记忆),自指独立注入模块已解 |

---

## B. 手势契约(设计基线,来自代码注释既有约定)

> 这些是判据要对着比的"本该"规格。摘自 `FreeGlobeController` 现有注释,非新增设计。

### 单指拖拽(契约 1.x)
- **1.2 起手判定一次**:抓到地表点 → 按 pitch 选 `Space`(拖球,绕地心锚点转)或
  `NearGround`(拖图,姿态锁定、锚点钉指下、Δpx 等量换算地表位移);整段不切换。
  抓不到(球外/天空)→ 回退 Space 的 spin 转台。
- **1.4 近地惯性**:像素速度样本按 iOS 权重 0.6/0.35/0.05 合成,<100px/s 不触发。

### 双指(契约 2.x)
- **2.2 组合手势轴独立**:PinchMode 只是"倾斜轴是否启用"的锁;缩放/旋转/平移由
  InputManager 每轴激活标志**独立门控**。**只有锚点钉合(pin)产生横向世界运动**,
  dolly/twist/pitch 不产生横向位移。
- **2.3 双指 pan 无惯性**(直接操纵,松手即停);仅缩放留足动量时启动 **zoom 惯性**滑行。

### 保锚退出(约束求解器)
- clampNow 传锚点 → 沿 **eye→anchor 直线**反向 dolly(dir/up 不变 ⇒ 锚点像素严格不动),
  牛顿三轮收敛。线近水平(`gain<0.2`)时退**径向抬升**——该位姿锚点已在掠射病态区,
  保锚判据本就不适用(C-V3 已量化此退化有界)。

---

## C. 技术债 / 结构债(明记,不假装没有)

| # | 债 | 影响 | 现状 |
|---|---|---|---|
| **C-P1** | **手势全链零自动化回归**:CAMPROBE 是手动真机工具,没进 ctest | 改相机代码(pin 解算/约束/惯性)只能手动注入+肉眼/手算验;回归靠人记得跑 | **未修,工具已就位**。做成"注入 pinch → 读 CAMPROBE → 断言 anchorErr<0.5px"需**真机在环**(host ctest 无 GPU/无相机窗口)。轻量替代:把 pin 解算/约束的纯数值部分抽出做 host 单测(不依赖真机),覆盖 C-V2/C-V4 的判据 |
| **C-P2** | **轴隔离 / 惯性收敛判据未量化**(C-V2/C-V4 挂 ❌ 的原因) | 组合手势串轴、惯性跑飞/早停这类回归**现在抓不到**——探针有数据但没有分析侧断言 | **未修**。CAMPROBE 已吐每帧 VP 矩阵(可反解 heading/pitch/roll/range),`camprobe.py` 补一段 pose delta 分析即可给 C-V2 出"pinch 期 Δheading≈0"之类数字;C-V4 需再吐 `inertiaAngularVelocity_`(tick 期)一个字段 |
| **C-P3** | **host ctest 不覆盖手势语义**:`tests/unit/camera/` 8 个测试(pose/flight/viewpoint/selector/tethered…)测的是位姿数学与控制器选择,**不碰 pin/anchorErr/惯性** | pin 正确性回归全压在真机手动上 | **部分可缓解**。pin 解算(`solveAnchorRotation`/`applyPinchPin`)、约束(`constrainEye` 沿线牛顿)是纯几何,**可脱离 GPU 做 host 单测**——给定 pose+finger 断言 anchor 投影落点。这是 C-P1 轻量替代的具体落点 |

---

## D. 已判死 / 勿再提(边界)

| 方案 | 死因 |
|---|---|
| 把径向抬升 fallback 改成"永远沿线保锚" | eye→anchor 线近水平时后退换不来高度、沿线无解;径向是唯一出路,且退化有界(C-V3 实测 ~0.4px 亚像素)、观感不受影响 |
| Cesium 式旋转补偿(俯仰/碰撞时偷改 direction 保锚点像素) | 会破坏 anchorErr 起手不变量、偷改视线;现设计选择"停住/拒绝"而非"事后顶起"(见 `rotateCameraVerticalAroundPoint` 注释与 [camera-constraint-chokepoint](记忆)) |
| move 期重 pick 地形定锚点高度 | 指下地形高≠抓取点高,法线对齐后锚点投影偏离手指(起伏越大/越斜越明显)=不跟手;正解=锚定在抓取球面同半径,一次旋转精确放回 |
| 用 sendevent/screenrecord 在真机自动化双指手势 | 零售 Oplus 无 root,`/dev/input`、`/sdcard`、`/data/local/tmp` 全被 SELinux 挡;正解=UiAutomation 注入(injector 模块) |

---

## 附:关联锚点

- 验证基建:`tools/cam_probe/README.md`(闭环三段链路)、`examples/android/injector/`(注入台)
- 相邻模块:约束求解细节散见 `CameraConstraintSolver`;无缝/加载期不在本文
- 记忆:`gesture-realdevice-harness-2026-08-18`(注入台+CAMPROBE+泄漏猎取全程)、
  `camera-architecture-2026-08-10`(真值按控制器分离)、
  `gesture-anchor-offray-jump-2026-08-01`(不跟手四项修)、
  `camera-constraint-chokepoint-2026-08-03`(resolveConstraints 唯一出口)
- commit:`569d4c535`(注入台)/ `16531b80c`(CAMPROBE 探针)/ `896edfc79`(eyeAlt 压测)
