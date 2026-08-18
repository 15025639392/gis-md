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
增益 1.000(C-V1);近碰撞退化 regime 已压测有界(C-V3);轴隔离 zoom/rotate 已量化
达成(C-V2);惯性收敛 flick + 近地**两条路径均已量化达成**(C-V4)。剩余唯一开放项是
**tilt 的掠射退化/灵敏度**(C-V5 观感待拍板)。pin 解算已有 host 单测兜底
(C-P3:anchorErr<0.5px 进 ctest);真机端到端闭环仍手动(C-P1)。判据侧已全部有数字兜底。

---

## A. 体验判据(C-V)

| # | 判据 | 类型 | 状态 | 代价 | 证据 / 差距 |
|---|---|---|---|---|---|
| **C-V1** | **pin 保锚**:手指下世界点跟手,pan/pinch/rotate 全程 anchorErr ≤ 1px | 机制 | ✅ | CAMPROBE 探针仅 debug 变体、每手势 START/MOVE/END 各一行 logcat,release 零开销 | **真机闭环坐实(`16531b80c`)**:平地 drag 43 帧末 0.01px / 峰 0.06px、**本该位移=实际位移=445.5px、增益 1.000**;pinch/rotate 峰 <0.05px。pin 解算求"让 anchor 投到 finger 的 pose",clampNow 沿 eye→anchor 线退出保锚 |
| **C-V2** | **轴隔离**:pinch 只动 range / rotate 只动 heading,不串轴(契约 2.2) | 机制 | ✅ | CAMPROBE 加 hdg/pit/range 字段(锚点 ENU 帧反解),debug-only | **真机量化(`camprobe.py` 轴Δ)**:**纯 zoom** → Δrange −55.9%、Δheading **+0.000°**、Δpitch **+0.000°**;**纯 rotate**(等距转)→ Δheading −33.6°、Δrange **0.00%**、Δpitch −0.028°。两个手势其余轴严格 <0.03°/0% = **隔离达成**。⚠️ **tilt 不在此判据**:tilt 的锚点在掠射退化区(anchorErr 峰 20px、range 度量爆表 +88km),轴隔离度量对它不适用 → 归 **C-V5**(tilt 是灵敏度设计非刚性锚) |
| **C-V3** | **近碰撞保锚上界**:高俯仰贴地形时保锚退化有界,anchorErr ≤ ~0.5px | 机制 | ✅ | 无额外开销(径向 fallback 是既有兜底) | **压测坐实(`896edfc79`)**:俯冲触底稳定卡 eyeAlt≈350m(地形~300+`kMinClearanceMeters`50)。高俯仰贴底 anchorErr 峰 ~0.4px(基线 7×)= `constrainEye` 末行**径向抬升 fallback 发火**(eye→anchor 线近水平 `gain<kAnchorExitMinVerticalGain=0.2`)。**亚像素、自限**(远锚点下径向 Δh 换算像素角漂本就小;更极端即抓取 miss 退 spin)。**非 bug**,验证注释"该 regime 保锚判据本就不适用" |
| **C-V4** | **惯性收敛**:fling 松手后**单调衰减**到停,不跑飞/不早停/不反向 | 机制 | ✅ | CAMPROBE tick 期吐 inertiaVel(flick)+ nearVel(近地),debug-only;注入台加 `oneFinger` 单指线性注入 | **两条路径均量化达成**:①**flick(Space,pitch<60°)**角速度 `1.430→…→0.000`(帧29),单调降、收敛停(指数 `exp(-kInertiaDampingPerSecond·t)`)。②**近地(NearGround,pitch≥60°)**像素速度 `200.4→175.4→…→11.8`(帧21),单调降、不回升、末 11.8px/s=0.2px/帧 <契约 1.4 停阈(0.5px/帧)→收敛停。近地触发需 `oneFinger`(单指线性恒速,`input swipe` 末端 ease-out 达不到 100px/s 释放阈值) |
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
| **C-P1** | **手势真机全链无自动化回归**:CAMPROBE 闭环仍是手动 | 端到端(真触摸→渲染→投影)回归仍靠人手动注入验 | **部分缓解**:pin 解算已有 host 单测兜底(C-P3),核心 anchorErr 不变量进 ctest 了。**剩**真机在环的端到端闭环(注入→CAMPROBE→断言)——需设备,重,暂靠手动 `tools/cam_probe` |
| **C-P2** | ~~轴隔离 / 惯性收敛判据未量化~~(已收敛) | — | **已修**:CAMPROBE 加 hdg/pit/range(C-V2)+ inertiaVel/nearVel(C-V4);注入台加 `oneFinger` 单指线性注入(触发近地惯性)。C-V2/C-V4 均已量化。**仅剩** tilt 的掠射退化度量(anchorErr 20px)——本属 C-V5 观感,非机制债 |
| **C-P3** | ~~host ctest 不覆盖 pin 语义~~(已补 pin) | — | **pin 已补**:`tests/unit/camera/test_pin_anchor_error.cpp`——直接构造 `FreeGlobeController`,高空 nadir(Space 良态)驱动 drag/pinch,**投影 grabbed 点断言 anchorErr<0.5px**(与真机 C-V1 同一不变量,脱 GPU)。区别于 `test_camera_pose_trace` 的整轨迹 hash——那是"没变",这是"钉对了"。**剩**:约束路径(`constrainEye` 沿线牛顿/径向 fallback)与惯性衰减暂无 host 断言,可照此扩 |

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
