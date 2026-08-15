# 架构沉淀:相机与手势 (camera + interaction)

> **架构/方案**文档。行号看 `AI_INDEX.md`(camera 节 / §17 interaction);行号随重构漂移,以符号名为准。手势系统**没有外部对照目标**,契约在本仓自定义并测试。标"未验证"处为 memory 既有断言、本文未逐行复核。

**规模**:`camera/` 3.7k 行 + `interaction/` 1.4k 行,旁及胶水层 `scene/SceneInteractionCoordinator`/`SceneInputCoordinator`。

---

## 职责边界

- **camera/**:相机的"输入 → 合法位姿"整条链路,三层职责严格分离:
  - `controllers/`(`FreeGlobeController` / `TetheredController` / `FlightController`)——**输入 → 位姿**,各自持真值,互不知道对方存在。
  - `CameraConstraintSolver`——**位姿 → 合法位姿**,纯策略执行者,不认识手势/惯性/飞行,只回答"给定 eye,合法的 eye 是什么"。
  - `CameraSystem`——编排层,把控制器族(`CameraControllerSelector`)与求解器接起来,跑帧循环、帧末哨兵、viewpoint 设定与只读派生量。
  - `Camera`(`scene/Camera.h`)——纯值对象:ECEF/世界米制、透视/正交两种投影、reverse-Z。不知道控制器/求解器/手势。
- **interaction/**:平台无关的手势识别 + 拾取 + 选中状态机,**完全回调解耦**——`InputManager` 不碰 Camera/Selection/GPU,只把归一化 `Gesture` 回调出去。`PickingService` 纯几何 CPU 拾取,无 GPU 回读。`SelectionManager` 只管选中/悬停状态。
- **胶水层**:`SceneInteractionCoordinator` 持 InputManager/PickingService/SelectionManager 三者;`SceneInputCoordinator` 是纯静态函数,把 `Gesture` 映射到 `CameraSystem`/`SelectionManager` 调用。

---

## 核心设计决策 + 理由

### 1. 相机真值按控制器分离,不是单一"位姿或轨道"表示 ★
- **Free 模式**:真值 = 世界位姿(`Camera::position/direction/up`)。理由:锚点钉合数学天然表述在世界位姿上;若改用 (focal, hpr, range),每个手势事件都要 pose→viewpoint 反解,而**掠视/视线不交地面时 focal 根本不存在**——恰是本项目手势数学领先参考基准的区间。
- **Tethered 模式**:真值 = `(frame, localHPR, range)`,世界位姿是**每帧派生量**。理由相反:载体动了而世界位姿不动就是脱钩;焦点由 frame 给定 ⇒ 反解良态。
- **同一系统里两种真值并存正是它们必须是两个类的原因**——焊进一个带 mode 分支的类会把两套数学缝进同一函数体。
- **已删除的历史反例**:orbit 表示(每帧 `lookAt(地心)` 重建)曾与位姿真值并存,直接产出过"双击天空→翻 orbit→下一帧重建强制看向地心→丢弃全部 tilt"的视角瞬移。**两套真值并存本身就是 bug 源**,是本决策最直接的反面教材。

### 2. 约束求解单一出口设计(chokepoint)
- 真实唯一入口是 `CameraConstraintSolver::constrainEye`,**只有两个调用点**:
  - `FreeGlobeController::clampNow`——手势/惯性路径,恒 `userDriven=true`、`deltaSeconds=0`。
  - `CameraSystem::resolveAtFrameEnd`——帧末哨兵,按位姿指纹判 `userDriven`,兜底收编所有未经 `clampNow` 路由的裸写(viewDistance/setNadirOrbitView/scriptedPan/Facade 或 JNI 绕过)。
  - ⚠️ **文档与代码命名有历史落差**:类注释仍写"当前 `CameraSystem::resolveConstraints`",但代码里并无此方法名,实际出口是 `constrainEye`。读代码以 `constrainEye` 为准。
  - 架构文档曾写"只有编排层能调 solver",已被 3a 重构否决:真正要防的两件事(钳位散落/绕过钳位)已由"solver 是唯一实现 + 帧末哨兵指纹"解决,那条额外约束反而在"手势事件内多步闭环"与"只能提议位姿"之间制造两难,是给不存在的问题设计的绕法。
- 理由:净空/near 的耦合常量(`kMinClearanceMeters=50`/`kNearFloorMeters=5`/`kNearSafetyRatio=0.5`)用 `static_assert` 锁定——这类不变量只有唯一出口才守得住,若钳位散落多处,任何一处漏改就是隐性回归。

### 3. 手势管线分层:能力接口 与 驱动接口分离
- `ICameraController`(每帧驱动相机的契约:`tick`/`onActivate`/`isAnimating`/`setViewport`)**刻意不含输入**。依据是读 Skybolt 源码:其 `setInput` 收归一化速率,对飞行模拟成立,对本项目的"直接操纵"是错的——"把抓住的地表点放回手指那个像素"所需信息就是那个像素坐标,归一化成速率恰好把它丢掉。
- `ITouchGestureTarget` **刻意与 `ICameraController` 分开**:后者"每帧驱动相机",前者"吃这一种输入"。`FlightController` 只实现前者(飞行不吃输入);Free/Tethered 两者都实现。编排层靠 `activeAs<T>()` 路由,返回 nullptr = 当前驱动者不吃这种输入,事件丢弃(如飞行中拖拽被静默吞掉)。
- `InputManager` 与相机完全解耦:只识别 drag/pinch/click 归一化手势并回调。桌面绑定把滚轮/中键/右键**合成为与双指完全相同**的 `Pinch*` 事件——"滚轮 zoom 与双指 zoom 走同一锚点通道"因此是**构造上成立**,不是两份实现凑巧一致。

### 4. Anchor 语义:钉合是唯一产生横向运动的机制,无意图分类 ★
- 核心不变量:单指拖拽先抓地表点、移动时让该点跟手;双指手势中**只有锚点钉合(pin)产生横向世界运动**,dolly/twist/pitch 数学上严格保锚、与 pan 正交——**没有意图分类**这一步,因为 pan/pitch 在输入端本不可区分(同向平移、间距/连线角不变),分类不可消除只能"起手一次 latch 定死"。
- 语义因控制器而异是设计不是缺陷:Free 下拖拽是"抓住地表点跟手",Tethered 下是"绕载体转"。
- **Cesium 式"旋转补偿"(SSCC `tilt3DOnTerrain`)明确不可引入**:它靠改 `direction` 把顶起后的位姿拉回,必然破坏 `anchorErr=0`——净空守卫只**拒绝"变更差"的方向**,不做事后旋转补偿式顶起("顶起要么破坏 Pitch 的锚点像素不变量,要么偷偷改 direction,都不如'停住'")。

### 5. openglobus reverse-Z 对齐
`Camera::ProjectionMode`(Perspective/Orthographic)**共用同一套 reverse-Z**:near→z_ndc=1、far→z_ndc=0、depth clear=0 + `GL_GEQUAL`,near/far 默认 1.0/1e12,显式对齐 openglobus `PlanetCamera`。切换透视/正交不需动任何深度状态(两条投影矩阵推导都收敛到同一 z_ndc 映射)。
> **健康态/故障态同读数陷阱**:`Camera::isOrthographic()` 一度**恒返回 false**,消费方看着像接好了实际永远走透视分支——`isOrthographic` 恒 false 与"没实现"读数完全一样,是这类 bug 最难查处。现已是真实状态。

---

## 数据流(关键路径)

```
平台输入 (UITouch/MotionEvent/鼠标)
  → InputEvent (归一化, 物理像素×devicePixelRatio)
  → InputManager::process()
      · 触摸: drag / processPinchWithPointerPair 起手 latch
      · 桌面: defaultDesktopBindings 命中 → emitSyntheticPinch(不走 latch, 鼠标意图由按键/滚轮显式给出)
  → InputManager::Callback(Gesture, InputEvent)
  → SceneInteractionCoordinator::onInputEvent (先 updateInteractionFocus, 再转发)
  → SceneInputCoordinator::handleGesture (静态函数)
      · Drag*/Pinch*/Click/DoubleClick → cameraSystem-> / pick+SelectionManager
  → CameraSystem 转发给 selector_.activeAs<ITouchGestureTarget>()
      (返回 nullptr = 当前驱动者不吃触摸, 事件在此丢弃, 如飞行中)
  → FreeGlobeController / TetheredController (吃到具体手势数学)
      · grabSurfacePoint / pointOnGrabSphere 保证锚点严格落在拾取射线上
      · applyAnchorDrag / pinch 保锚数学改世界位姿真值
      · 每次改完位姿立刻调 clampNow(&anchorWorld)
  → clampNow → CameraConstraintSolver::constrainEye (唯一钳位实现)
      · 近场探针 / 非对称滤波 / 碰撞钳位 / 保锚退出
  → Camera::setView → solver_->commitPose (位姿指纹更新, 供帧末哨兵判 user-driven)

（每帧独立于手势）
CameraSystem::update(dt)
  → beginFrame → updateInternal(dt): active controller tick(惯性/飞行/系留每帧推进)
  → resolveAtFrameEnd(dt): 位姿指纹比对 → constrainEye 兜底钳位裸写
```

飞行路径不吃输入,`flyTo` 直接 select 切控制器,`tick` 按缓动曲线推进,`consumeCompleted` 在 `updateInternal` 之后交还 Free。

---

## 关键契约与不变量

| 契约 | 说明 |
|---|---|
| `constrainEye` 是唯一位姿合法性出口 | 任何其他直接改 `Camera` 位姿又不经它的写法都是绕过;帧末哨兵是兜底"实在绕不开"的裸写,不是给新代码开口子。新增"设相机位置"路径必须走 `clampNow` 或让帧末哨兵兜底,不能自己再调地形采样/钳位 |
| 净空↔near 耦合 static_assert | `kNearFloorMeters ≤ kMinClearanceMeters × kNearSafetyRatio`,禁止单独改一个常量 |
| anchorErr 判据 | 锚点当前投影 − 手指像素应恒 0(起手帧尤其)。⚠️ 专用 `GESTDIAG` 插桩已在 pinch 重构后**整体移除**(只剩注释),真机 p95=0.0px 历史基线**当前无法直接复验**;排查新锚点漂移需先重新插桩 |
| Cesium 旋转补偿必破 anchorErr,勿引入 | 任何"位姿越界后靠转动 direction 拉回"都会打破像素级锚定;正解是越界前拒绝该步进,不是解出非法位姿后事后修正 |
| 锚点必须落在拾取射线上 | `PickingService::pickTerrain` 返回点本身**不在射线上**(先椭球求交再按地形高抬起);修法在消费侧:`grabSurfacePoint` 只取拾取点半径,再用同一像素射线与该半径球重新求交(`pointOnGrabSphere`)。**任何新写的"拿 pick 当锚点"代码都必须重复这步**,否则复现起手跳变 227~471px |
| `isSelfAnimating()` 必须含飞行期 | `scriptedPanActive_ \|\| (active && active->isAnimating())`,只报"自主演进"不报"手指按着"——两条判定故意分开,否则出现两不管的缝 |
| 飞行期 `cameraMoving` 恒真饿死瓦片 | `cameraMoving` 纯按帧间位移>2m 判定、**不区分驱动源** ⇒ 飞行期恒真 ⇒ `cullRequestsWhileMoving` 全程延迟请求 ⇒ 飞到目的地画面空。修法不是关 cameraMoving,而是 `cameraFlightActive/Progress` 喂 FrameState,减速段(progress>0.7)豁免 cull。对齐 Cesium `canPreloadFlight()`。⚠️ **飞行不豁免碰撞净空**——靠路径规划期预采地形抬高拱高使钳位结构性不触发 |
| 控制器切换零跳变靠自重建 | 切换先 `onDeactivate` 旧、再 `onActivate` 新(顺序是契约);`onActivate` 不带参,新控制器必须自己从 `Camera` 当前状态重建内部真值,不能假设上一个控制器留了状态 |

---

## 诚实得失

### ✅ 强项
- **锚点跟手数学与约束体系判据先行,明确"不落后于参考基准"**:Cesium 低空 `pan3D` 是屏幕空间启发式带经验因子、碰撞只取相机正下方单点 + 对称 10% 滤波;本项目非对称滤波(上升/小变动立即,大幅下降 τ=0.5s 指数逼近)+ 近场探针(中心+三环×8方位+扫掠走廊)在同类判据上更精细。
- **桌面输入绑定表是"翻译层不是第二套相机数学"的构造性保证**——滚轮/中键/右键合成为与双指字面相同的 Pinch 事件,"走同一锚点通道"是构造上不可能不一致。
- **单一出口经过一次真实的架构自我纠错**:曾写进文档的"只有编排层能调 solver"被后续重构证明是给不存在的问题设计的绕法,已否决并留档——说明约束体系有真实的反例驱动收敛过程。

### ⚠️ 短板 / 已知债
- **飞行期饿死瓦片是模式性坑,不是孤立 bug**:`cameraMoving` 这类"纯按位移、不分驱动源"的判定,每新增一种相机自主运动(飞行/回中/脚本平移)都要重踩一遍。当前只在飞行路径打了专门补丁,**没做成通用的"驱动源标签"机制**——未来再加一种自主运动大概率重踩"画面到目的地是空的"。
- **手势系统自定义契约,缺少外部对照系持续验证**:`GESTDIAG` 插桩在一轮真机验收后被整体移除,anchorErr 这条核心判据**现在只存在于历史真机验收记录里,代码里没有常驻可复验探针**。下一次怀疑锚点漂移得先重新插桩才能定位——这是"手势没有外部对照目标,只能自证"的直接代价。
- **`kMaxDistanceEarthRadii` 仍只是调用点的闸,未进 `constrainEye` 统一出口 ⇒ 无测试覆盖**(未验证)。
- **三档惯性清零行为不统一**(`viewDistance` 只清 pan、`resetNorthUp` 连 zoom 一起清)是拆分前遗留现状、不是设计,统一它属行为变更需单独一刀(未验证当前状态)。
- **正交模式只是渲染层能力,手势/瓦片两侧还没跟上**:SSE 仍假定透视投影(正交下瓦片 LOD 是错的);转台增益读 `verticalFovRadians`(正交下无意义)。文档明确这是非目标而非遗漏。

---

## 扩展点
- **加新手势**:`InputEvent::Type` 加事件 → `InputManager` 识别为新 `Gesture` → `SceneInputCoordinator::handleGesture` 加分支 → `CameraSystem` 转发给 `activeAs<ITouchGestureTarget>()`。**不要**往 `ICameraController` 加通用输入钩子(AI_INDEX 明确"别再往接口里加")。新手势只有某控制器关心就新增到 `ITouchGestureTarget` 或控制器私有方法,不为复用硬凑接口。
- **加新相机模式(新控制器)**:实现 `ICameraController`(必要时同时实现 `ITouchGestureTarget`),`selector_.add` 注册。先想清"真值是什么"(世界位姿 or 派生量),仿照 Free/Tethered 决策,不默认抄 Free。新增第三个真实现时应重复"重新复核 `ICameraController` 接口"这一验收动作(假控制器只能证明契约没夹带 Free 私有假设,证明不了够用)。
- **加新约束**:一律加进 `constrainEye` 内部或它调用的探针/滤波函数,不在控制器/CameraSystem 单独实现钳位。需额外数据源走 solver 现有注入口模式(`setXxxFunc`),保持 solver "纯策略执行者"边界。
- **飞行豁免类新契约**:任何新的相机自主运动若要在瓦片消费侧豁免 `cameraMoving`,应仿照 `cameraFlightActive/Progress`——由 `CameraSystem` 暴露只读派生量喂进 `FrameState`,瓦片侧显式按 progress 判豁免窗口,**而不是直接改写 `cameraMoving` 的计算**(共享字段,牵一发动全身)。

---

## 对照系
- **相机(camera/)**:对标 openglobus `PlanetCamera`(reverse-Z)和 Skybolt `CameraControllerSelector` 接口形状,但**刻意偏离** Skybolt 的 `setInput` 归一化速率(判定"对直接操纵是错的")。约束/碰撞对标 Cesium `pan3D`/单点碰撞+对称滤波,判定本项目更精细(此判据来自 memory survey,未重核 Cesium 当前实现,标未验证)。桌面绑定对标 Cesium `ScreenSpaceCameraController` 但**刻意不做** Shift+左键=look。
- **拾取(PickingService)**:`rayTriangleIntersection` 对标 cesium-native `IntersectionTests::rayTriangle`(Möller–Trumbore)。
- **手势管线**:**没有外部对照目标**——部分借鉴 MapLibre 的"起手快照+单次 latch"思路,但锚点数学、病态区连续化混合、latch 判据均为自研。这意味着**手势正确性只能自证**(对拍台 hash、anchorErr 探针、真机数字键回放),没有"和参考实现比对"这条验证路径——这也是"插桩被移除后无常驻可复验探针"格外值得关注的原因:失去插桩=失去唯一的(准外部)验证手段。
