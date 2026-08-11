# 相机系统架构设计

> 状态：设计稿 v2（Q1=通用 GIS SDK / Q3=双击天空不响应 已拍板）
> 基准：`.ref/cesium-js`(Camera.js 3989 / ScreenSpaceCameraController.js 3110 / CameraFlightPath.js 576)、
> `.ref/osgearth`(EarthManipulator 4558 / Viewpoint 310)、`.ref/Skybolt`(CameraController 多态族)

---

## 0. 一句话

现状不是"手感差"——手势数学和约束体系我们领先基准；问题是**相机不是一个可编程对象**：
对外能改视角的 API 只有 3 个，`CameraController` 是 1556 行、7 职责的 god class，
靠一个 `orbitMode_` 布尔在两套状态真值之间切换。

本设计做四件事：**定真值 → 拆职责 → 上多控制器 → 补装配**。

---

## 1. 现状问题（压缩自 survey）

| # | 问题 | 证据 |
|---|---|---|
| P1 | 状态双表示：`rotation_+distance_`(orbit) vs 位姿(free)，`orbitMode_` 切换 | `rebuildOrbitPose()` 强制 `lookAt(地心)` |
| P2 | 双击天空 → 视角瞬移到正俯视（可复现 bug） | `SceneInputCoordinator` 双击 miss → `setDistance` → 翻 orbit → 丢弃全部 tilt |
| P3 | 无 viewpoint API：无 heading/pitch/roll 写入、无 roll 概念、无飞行、无框选范围 | 对外仅 `setDistance`/`setNadirOrbitView`/`viewDistance` |
| P4 | god class：手势 + 惯性 + 探针 + 滤波 + 约束 + orbit 重建 + 测量台脚本 | `CameraController.cpp` 1556 行 |
| P5 | 驱动源仲裁隐式：清零惯性的逻辑散在 4 处调用点 | `onDragStart`/`onPinchGesture`/`setMeasurementFreeze`/`setScriptedPan` |
| P6 | `far` 恒 1e12 从未被改写 ⇒ Frustum 远平面裁剪恒不生效 | `Camera` 构造函数是唯一赋值点 |
| P7 | 无参考系概念 ⇒ 做不了跟踪移动载体（通用 SDK 必需） | 全部数学硬编码 ECEF 全局系 |
| P8 | `isOrthographic()` 恒 `return false` ⇒ 无正交投影 | `Camera.h` |

**不在问题清单里的（我们领先，不要"补齐"回退）**：全海拔精确锚点钉合（Cesium 低空 `pan3D` 是屏幕空间启发式，
带 `max(tan(angle),0.1)` 钳位和经验因子 `(1-dot)`）、区域探针 + 扫掠走廊碰撞（Cesium 只取相机正下方单点
`scene.globeHeight`）、非对称突变滤波（Cesium 对称 10% 阈值 ⇒ 瓦片抬高地形时延迟生效=穿模）、
`resolveConstraints` 单一出口 + 帧末指纹哨兵、保锚退出方向（沿 eye→anchor 牛顿迭代）。

---

## 2. 根决策：真值按控制器分离，统一输出世界位姿

### 2.1 三个基准的选择

| 引擎 | 组织方式 | 真值 | 代价 |
|---|---|---|---|
| Cesium | 单个 `Camera` 塞 `transform` 矩阵 | frame-local 位姿 + WC 派生 | 所有消费者要区分 local/WC；`_setTransform(IDENTITY)` save-restore 遍布（`adjustHeightForTerrain` 里就有一段） |
| osgEarth | 单个 `EarthManipulator` | `_center+_rotation+_distance` + `_tetherNode` | pan 只能绕 `_center` 转，锚点数学无处安放 |
| Skybolt | **多控制器 + `CameraControllerSelector`** | 每个控制器自持 | 多一层间接 |
| **本设计** | **多控制器 + selector，统一输出世界位姿** | 见 2.2 | 多一层间接 |

### 2.2 定案：每个控制器持有对它良态的表示，统一产出世界位姿

```
FreeGlobeController   真值 = 世界位姿 (eye/dir/up)          ← 自由浏览，锚点钉合
TetheredController    真值 = (frame, localHPR, range)       ← 跟踪/固连载体
FlightController      真值 = (起终 viewpoint, 进度)          ← 过渡态，飞完交还
        ↓ 全部产出 ProposedPose（世界系）
CameraConstraintSolver → Camera（唯一世界位姿真值）
```

**为什么 Free 模式不能用 (focal, hpr, range)**（可反驳的中间步骤）：

1. 锚点钉合数学上是"求绕地心的旋转把锚点放回手指像素"，直接作用于位姿。
2. 若真值是 (focal, hpr, range)，每个手势事件要做 pose→viewpoint 反解，
   **该反解在掠视 / 视线不与地面相交时病态**——focal 不存在、range 无定义。
   掠视恰是我们花力气最多、做得最对的区间（条件数加权转台混合）。
3. osgEarth 能用 focal，是因为它的 pan **就是**绕 `_center` 转，它没有这套数学。抄它=放弃优势。

**为什么 Tethered 模式反过来必须用 (frame, localHPR, range)**：

4. tether 下焦点由 frame 给定，**不需要靠视线求交**——第 2 条的病态原因消失，反解良态。
5. 而世界位姿表示在 tether 下反而是错的：载体动了，位姿不动就脱钩。真值必须是相对量。

**结论：这不是"选一个"的问题，是两种模式各自的良态表示不同。**
单一表示必然在某一侧凑合——Cesium 用 transform 矩阵抹平差异，代价是全局 local/WC 二义性。
多控制器把差异关在各自类里，代价只是一层间接。

### 2.3 关键不变量

- **`CameraConstraintSolver` 是唯一的钳位实现。** 调用点可以有多个（手势期是
  事件内多步闭环：dolly→twist→pitch→pin 每步都要在合法位姿上继续），但钳位
  逻辑只有这一处。
- **绕过钳位直接写 `Camera` 由帧末哨兵的位姿指纹兜底**，不靠"禁止调用"来防。
- **`onActivate(currentWorldPose)`：控制器接管时从当前世界位姿初始化自身状态 ⇒ 切换零跳变。**
  Tethered 接管时的反解是良态的（frame 已知）；Free 接管时直接存位姿，恒良态。

> ⚠️ 修订记录：本节初版写的是"所有控制器只提议位姿、`CameraSystem` 是唯一调用
> solver 的人"。那条**已否决**（74692775b）。它要防的两件事上面两条已经解决，
> 它自己却制造了"事件内多步闭环 vs 只能提议位姿"的两难——回调注入和返回提议
> 列表两种绕法，都是在给不存在的问题设计绕法。去掉它是净删除。**别再加回来。**

### 2.4 推论：orbit 模式整体删除

`rotation_` / `distance_` / `orbitMode_` / `rebuildOrbitPose()` 全部退役。
它们存在的唯一理由是"缺 viewpoint API 时代的位姿设定替代品"——补上 §4 后理由消失。
删除同时根治 P1、P2。

生产消费者只有 3 处（已 grep 确认）：

| 消费者 | 现状 | 迁移后 |
|---|---|---|
| `SceneInputCoordinator` 双击 pick miss | `setDistance(distance()*0.7)` | **不响应**（已拍板：指向天空的双击没有地理语义） |
| `EarthEngineSdkFacade::resetCamera` nadir 分支 | `setNadirOrbitView` | `setViewpoint({targetGeo, heading:0, pitch:-π/2, range:h})` |
| `test_camera_controller.cpp` ~15 处 | `setRotation`/`setDistance` 摆位 | `setViewpoint` 摆位 |

---

## 3. 分层与职责

```
earth_engine/camera/
  Camera.h/.cpp                 位姿 + 投影数学（+ ProjectionMode，见 §8）
  CameraPose.h                  位姿值类型 + ↔ (geo, heading, pitch, roll) 互转
  Viewpoint.h                   接口层表述，字段全 optional + 参考系
  CameraConstraintSolver.*      地形探针 + 突变滤波 + 碰撞钳位 + groundState   ← 抽 ~450 行
  controllers/
    ICameraController.h         接口：onActivate / onDeactivate / tick / isAnimating / setViewport
    FreeGlobeController.*       锚点钉合 + 双指 + 惯性                        ← 抽 ~750 行
    TetheredController.*        frame 相对 HPR + range                        ← 新 ~200 行
    FlightController.*          路径 + 缓动 + tick，飞完交还目标控制器          ← 新 ~280 行
  CameraControllerSelector.*    切换 + onActivate 零跳变                       ← 新 ~80 行
  CameraSystem.*                门面：输入路由 / 约束单一出口 / 帧末哨兵 / 测量台  ← ~250 行
```

**`CameraController` 更名 `CameraSystem`**：语义已从"控制器"变成"编排器"，
且 `CameraController` 这个名字要留给控制器族的基类概念。
（"手术式修改原则"的例外条款适用——本任务就是整体改造。）

> ⚠️ 修订记录（0fe21163b / 本节两处）：
>
> 1. **转发头不做。** 本节初版写"保留一版转发头给外部调用方过渡"。grep 全仓确认
>    `CameraController` 没出现在任何公开 SDK 头（`EarthSceneConfig.h` 里只是注释），
>    调用方全在本仓库内 ⇒ 转发头是凭空的债，直接改名即可。
> 2. **接口里没有 `onGesture`（原文写的是 `evaluate / onGesture / onActivate`）。**
>    Skybolt 的 `CameraController::setInput` 收归一化速率（`forwardSpeed`/`yawRate`/
>    `zoomRate`），对飞行模拟式速率控制成立，对我们的**直接操纵**是错的——"把抓住的
>    地表点放回手指那个像素"所需的全部信息就是那个像素坐标，归一化成速率恰好把它
>    丢掉，而那正是我们相对基准领先的部分。而且各控制器输入形状本就不同（Free 吃
>    触摸、桌面吃滚轮/中键、Flight 根本不吃），硬凑公共输入接口只会得到一个谁都要
>    现场解释的联合体。
>    ⇒ **输入路由到具体类型**，`CameraControllerSelector::activeAs<T>()`
>    （Skybolt 自己也是这么逃生的：`getControllerOfType<T>()`）。**别再加回去。**
> 3. `onActivate` **不带参**（原文 `onActivate(currentWorldPose)`）：控制器本就持
>    `Camera*`，位姿从那里读，再传一份是纯冗余。契约不变。

职责边界，每个类问一句：

| 类 | 它知道什么 | 它**不**知道什么 |
|---|---|---|
| `Camera` | 位姿、投影模式、FOV/正交宽度、near/far、矩阵、pick ray | 地球、地形、手势、时间 |
| `CameraConstraintSolver` | 椭球、地形采样回调、净空常量 | 手势、动画、控制器、输入事件 |
| `FreeGlobeController` | 抓取球、锚点、惯性、条件数混合 | 地形高度（只经 solver 拿结果）、飞行、tether |
| `TetheredController` | frame provider、local HPR、range | 锚点数学、地形 |
| `FlightController` | 起终 viewpoint、路径曲线、缓动、进度 | 手势、碰撞细节 |
| `CameraSystem` | 谁在驱动、测量台覆盖、何时解约束 | 上面任何一个的内部数学 |

---

## 4. 核心数据结构

### 4.1 参考系（替代 Cesium 的 `camera.transform` 矩阵）

```cpp
struct ViewpointFrame {
    /// 参考系原点。nullptr = 世界系(ECEF)。返回 false = 目标暂不可用，保持上帧。
    std::function<bool(Vec3& outOriginEcef)> originProvider;
    /// 参考系姿态。nullptr = 用原点处的 ENU（东-北-天）。
    /// 提供时 = 完全固连载体机体系（roll 跟随载体）。
    std::function<bool(glm::dquat& outOrientation)> orientationProvider;
};
```

三档表达力，覆盖通用 SDK 的全部 tether 需求：

| origin | orientation | 语义 |
|---|---|---|
| null | null | 地理 viewpoint（自由浏览） |
| 有 | null | 跟踪目标位置，视角仍按地理 ENU（跟车但保持北上） |
| 有 | 有 | 完全固连载体机体系（座舱视角、roll 跟随） |

**为什么不用 4×4 矩阵**：表达力等价，但矩阵形式会诱使 `Camera` 持有它（Cesium 的路），
从而让全局代码陷入 local/WC 二义性。provider 形式把参考系关在 `TetheredController` 内部，
`Camera` 只见世界位姿，solver / 渲染 / 拾取全部无感。

### 4.2 Viewpoint（抄 osgEarth 的 optional 语义）

```cpp
struct Viewpoint {
    ViewpointFrame frame;                    // 缺省 = 世界系

    // ---- 位置：eyeGeo 与 (targetGeo + range) 二选一 ----
    std::optional<Cartographic> eyeGeo;
    std::optional<Cartographic> targetGeo;
    std::optional<double>       rangeMeters;

    // ---- 朝向（在 frame 内）：缺省 = 保持当前 ----
    std::optional<double> headingRadians;    // 0 = 正北/正前，顺时针为正
    std::optional<double> pitchRadians;      // 0 = 水平，-π/2 = 正俯视
    std::optional<double> rollRadians;
};
```

**每个字段 optional ⇒ "部分 viewpoint" 语义**，这是全设计性价比最高的一处：

| 需求 | 表达 |
|---|---|
| 现 `resetNorthUp()` | `setViewpoint({.headingRadians = 0})` |
| 现 `setNadirOrbitView` | `{.targetGeo=g, .headingRadians=0, .pitchRadians=-π/2, .rangeMeters=h}` |
| 现 `viewDistance` | `{.targetGeo=g, .rangeMeters=d}`（hpr 缺省保持） |
| 新：只改俯仰 / 只给 roll | `{.pitchRadians=-π/4}` / `{.rollRadians=r}` |
| 新：跟踪飞机、保持北上 | `{.frame={.originProvider=f}, .rangeMeters=500}` |
| 新：座舱视角 | `{.frame={f_pos, f_att}, .rangeMeters=0, .headingRadians=0}` |

反解 `currentViewpoint()`：`eyeGeo`/`heading`/`pitch`/`roll` **恒可解**；
`targetGeo`/`rangeMeters` 需视线与地面求交，不交时返回 `nullopt`——诚实表达"当前没有焦点"，
不要伪造一个地平线外的假焦点（这正是 §2.2 第 2 条的同一个理由）。

### 4.3 CameraPose 互转

```cpp
struct CameraPose {
    glm::dvec3 eye, direction, up;
    static CameraPose fromFrame(const glm::dvec3& origin, const glm::dquat& frameOrient,
                                double heading, double pitch, double roll, double range);
    void toFrame(const glm::dvec3& origin, const glm::dquat& frameOrient,
                 double& outH, double& outP, double& outR, double& outRange) const;
};
```

实现复用现成的 `Transforms::enuToEcef` / `ecefToEnu`（`TerrainDisplacementTemplate` 已在用）
和现有 `headingFromFrame()` / `pitchRadians()`。**不新造数学。**

---

## 5. 驱动仲裁（两层，比原方案的 5 级优先级表干净）

现状：清零惯性的逻辑散在 4 处，各写各的。收成两层：

| 层 | 谁 | 规则 |
|---|---|---|
| 覆盖层 | `Script` / `MeasurementFreeze` | 测量台，覆盖一切；freeze 时 `observeOnly`（位姿逐帧字节稳定） |
| 驱动层 | **selector 选中的唯一控制器** | 同一时刻有且仅有一个；切换走 `onActivate` 零跳变 |
| 控制器内部 | 手势 vs 惯性 | 各控制器自己管，不上升到系统层 |
| 兜底 | `External` 裸写（Facade/JNI） | 帧末哨兵指纹检测后按 user-driven 收编 |

切换规则：
- 手势事件到达 → 若当前是 `Flight`，取消飞行并切回飞行前的控制器（手势永远优先）
- `flyTo(viewpoint)` → 切 `Flight`，飞完 `selector.select(飞行目标所隐含的控制器)`
  （目标 viewpoint 带 frame provider ⇒ 落到 `Tethered`；否则落到 `Free`）
- **`kMinClearanceMeters` 净空是硬不变量，任何控制器不豁免**——不能穿地不因"是程序在飞"松掉

---

## 6. 位姿钳位的两条路径

帧末指纹哨兵是本次要**保住**的资产。变化是把原先挤在一个
`resolveConstraints(ConstraintContext)` 里的两条路径拆开——那个 context 的
`source` 枚举唯一作用就是区分"是不是帧末"，六个调用点各自现场装配一个 3 字段
结构只为传这一个 bit。

| 路径 | 谁调 | 语义 | user-driven | dt | pinnedAnchor |
|---|---|---|---|---|---|
| `clampNow` | 手势 / 惯性（控制器内，直呼 solver） | 我刚动了相机，钳一下 | 恒 true | 0 | 有 |
| `resolveAtFrameEnd` | `CameraSystem` 帧末 | 检查有没有人绕过我 | 指纹判定 | 真实 dt | 无 |

```
每帧 CameraSystem::update(dt)
  ├─ 1. solver.beginFrame()（探针"每帧至多重建一次"的时钟）
  ├─ 2. 覆盖层检查（Script / Freeze）
  ├─ 3. selector.current()->evaluate(dt)   ← 控制器内部按需自行 clampNow
  └─ 4. resolveAtFrameEnd(dt)：指纹比对，收编绕过的裸写
```

**手势期必须在事件内闭环钳位**——延到帧末锚点会漂。
**`clampNow(pinnedAnchor)` 是保锚退出方向的输入，这条线不能在拆分中丢。**

---

## 7. 飞行子系统

### 7.1 路径

用现成的 `SimplePlanarEllipsoidCurve::fromTwoPositions(source, destination)`
（`getPosition(percentage, additionalHeight)`，目前**零引用**，现成可用）。

拱高 `additionalHeight` 取三者最大：能同时看见两端所需高度、路径地形采样最大高 + 净空（见 7.3）、0。

### 7.2 时长与缓动

时长启发式抄 Cesium（`CameraFlightPath.js`）：

```
duration = min(ceil(distance_meters / 1e6) + 2.0, 3.0)   // 秒
```

缓动 QUINTIC_IN_OUT（Cesium 默认），一个函数即可，不引库。
朝向插值：heading/pitch/roll 各走最短弧（heading 要 unwrap 过 ±π）。

### 7.3 与瓦片系统的耦合（最容易漏的一条）

`TileFrameInteractionTracker::evaluate` 现在这样判定：

```cpp
snapshot.cameraMoving = snapshot.cameraPositionDeltaMagnitude > 2.0;  // 米/帧
```

**纯按位移、不区分驱动源。** 飞行期每帧位移上千米 ⇒ `cameraMoving` 全程 true
⇒ `cullRequestsWhileMoving` 全程延迟请求 ⇒ **飞到目的地画面是空的**。
Cesium 用 `Camera.canPreloadFlight()` 处理同一件事。

契约（本次必须一并落）：
- `FrameState` 增加 `cameraFlightActive` / `cameraFlightProgress`
- 飞行减速段（进度 > 0.7）关闭 `cullRequestsWhileMoving`，让目的地瓦片提前进队
- 路径规划期沿曲线采样地形（复用 `TerrainAreaSampleFunc`），最大高 + 净空抬进拱高，
  使碰撞钳位在飞行期**结构性不触发**——而不是靠飞行期豁免约束

---

## 8. 正交投影（Q1=通用 SDK ⇒ 纳入）

`Transforms::createOrthographicMatrix` 已存在。**相机层要做对的四件事**：

1. `Camera` 加 `ProjectionMode {Perspective, Orthographic}` + `orthographicWidthMeters`；
   `isOrthographic()` 改成真实实现（现在恒 `false`）
2. `projectionMatrix()` 分支；正交仍走 reverse-Z 约定（与现有深度约定一致）
3. `getPickRay()` 正交下是**平行射线**：origin 随像素平移，direction 恒为 `camera.direction`
   ——现在的 unproject 实现在正交下会给出错误的 origin
4. `frustum()` 正交下是盒子；`SceneFrameUpdateCoordinator` 的**动态 near 公式在正交下无意义**
   （正交没有 z_ndc 病态区），必须分支，否则会算出荒谬的 near

**不做**（明确非目标）：场景级 2D 模式——墨卡托平面下的瓦片选择、拾取、渲染路径适配，
以及 Cesium 式 3D↔2D morph 过渡动画。那是独立项目，本设计只保证相机层不堵死它。

---

## 9. 与既有系统的契约（不许在重构中打破）

| 契约 | 现持有者 | 拆分后归属 |
|---|---|---|
| `groundState().nearestGeometryMeters` → 动态 near | `CameraController` | `CameraConstraintSolver`，`CameraSystem` 转发 |
| 净空 ↔ near 的 `static_assert` 耦合 | `CameraController.h` | 随 solver 走，断言原样搬 |
| `isSelfAnimating()` → 帧级按需渲染 | `CameraController` | `CameraSystem`（**必须加上飞行期**） |
| `measurementFreeze` 逐帧字节稳定 | `CameraController` | `CameraSystem`，语义不变 |
| `setScriptedPan` 测量台脚本 | `CameraController` | `CameraSystem` 覆盖层 |
| `headingRadians()` → 指北针 | `CameraController` | `CameraPose` + `CameraSystem` 转发 |

⚠️ `isSelfAnimating()` 漏掉飞行期 = "飞到一半停帧冻住"，和它当初漏掉 pan 惯性是同一个坑。

---

## 10. 落地阶段与验收判据

每阶段自身自洽、可单独合入。**不是"先简单后重构"**——每阶段都是最终形态的真子集。

| 阶段 | 内容 | 二元验收判据 |
|---|---|---|
| 1 | 拆分 + selector 骨架（只有 `FreeGlobeController` 一个实现），零行为变更 | `ctest` 全绿（现 179/179）；真机 anchorErr p95 = 0.0px 不变；同输入序列逐帧 `groundState` 与重构前逐位相同（临时对拍） |
| 2 | `CameraPose`/`Viewpoint`（含 frame 字段，provider 可空）+ 删 orbit | `setViewpoint→currentViewpoint` 往返恒等（病态返回 nullopt）；P2 回归：双击天空后位姿逐位不变；hpr 单独写入互不串扰 |
| 3 | `FlightController` + 路径地形采样 + `cameraFlightActive` 契约 | 终点位姿相对误差 < 1e-3；穿山路径全程 AGL ≥ 净空；落地帧目的地瓦片就绪率 ≥ 静止同位姿的 90%；`isSelfAnimating()` 飞行期恒 true |
| 4 | `TetheredController` + frame provider | 载体移动 1000 km 后相对位姿（localHPR+range）逐位不变；Free↔Tethered 切换帧位姿连续（跳变 < 1e-6 m）；载体姿态变化时 roll 跟随（orientationProvider 用例） |
| 5 | `Camera` 正交投影接线 | 正交下 pick ray 与透视在同一像素的地面命中点一致（相机足够远时）；正交下 near 不再走透视公式；`isOrthographic()` 真实反映状态 |
| 6 | 桌面输入绑定表（`InputEvent` 加 `Wheel` + Cesium 式 `EventType × Modifiers` 映射） | 滚轮 zoom 与双指 zoom 走同一锚点通道（anchorErr 判据一致）；中键 tilt 与双指 Pitch 同语义 |

工程量（AI 协作基准）：阶段 1~3 各半天到一天，阶段 4~6 各半天。
主要成本在阶段 1 的对拍验证，不在写代码。

---

## 11. 已拍板 / 未决

**已拍板**
- Q1 目标形态 = **通用 GIS SDK** ⇒ 参考系（§4.1）、正交（§8）、桌面输入（阶段 6）全部纳入
- Q3 双击天空（pick miss）= **不响应**

**未决（不阻塞阶段 1~3，需要在阶段 4 前给答案）**
- N1 场景级 2D 平面地图模式的优先级——它是独立项目（瓦片选择/拾取/渲染全链），
  本设计只保证相机层不堵死。要不要排期？
- N2 `TetheredController` 的手势语义：tether 下双指该改 `range` 还是改 local pitch？
  osgEarth 的 `TetherMode` 有三种（跟随朝向 / 仅跟随位置 / 跟随并保持视角），要不要照搬？
- N3 `camera.changed` 事件是否对外暴露（Cesium 有 `percentageChanged` 节流）。
  内部已有 `cameraMoving` 位，对外暴露是纯装配。

---

## 附：与基准的对照小结

| 维度 | Cesium | osgEarth | 本设计 |
|---|---|---|---|
| 组织 | 单 Camera + 单 SSCC 巨类 | 单 Manipulator 巨类 | 多控制器 + selector（Skybolt 式） |
| 真值 | frame-local 位姿 + transform | focal+hpr+range | 按控制器分离，统一输出世界位姿 |
| 参考系 | 4×4 transform 矩阵（全局 local/WC 二义） | `_tetherNode` + `_tetherRotation` | provider 回调，关在 Tethered 控制器内 |
| 接口表述 | `setView({destination, orientation})` | `Viewpoint`（全 optional） | `Viewpoint`（全 optional + frame） |
| 模式切换跳变 | 手工处理 | 手工处理 | `onActivate(currentWorldPose)` 结构性消除 |
| 碰撞 | 单点 nadir + 对称滤波 | 简单钳位 | 区域探针 + 扫掠走廊 + 非对称滤波（**保持**） |
| 跟手 | 低空屏幕空间启发式 | 绕 focal 转 | 全海拔精确锚点钉合（**保持**） |
