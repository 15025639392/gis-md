# 手势系统深度调研 — Android 基准 vs 成熟地球引擎

日期：2026-07-06 · 基准平台：Android · 对照：Cesium(JS `ScreenSpaceCameraController` / `Camera`)、Google Earth/Maps、Mapbox GL

---

## 0. TL;DR

本引擎的交互链路是一条**分层干净、Cesium 风格的锚点球面相机**实现：Java/JNI 只做多点整形 → 归一化 `InputEvent` → C++ `InputManager`(手势状态机) → `CameraController`(四元数 orbit + 自由态混合)。算法核心（锚点拖拽、双精度四元数、地形高度碰撞快路径、速度惯性）是**生产级、忠实**的。

但与成熟地球引擎相比，差距集中在**交互丰富度与鲁棒性**，不是算法正确性。最关键的 5 条：

1. **地平线/掠射角死区**（P0 观感）——单指拖到球体边缘/仰视时 pick ray 打空，drag 直接 `return`，地图卡死不动。Cesium 有 `spin3D` 回退（打空时按像素位移恒速旋转），本引擎没有。
2. **平台严重不对等**——iOS 缺双指旋转+倾斜（没接 `UIRotationGestureRecognizer`），macOS `MetalView` **零输入处理**（纯显示，无鼠标滚轮/右键拖拽/键盘）。Android 是唯一功能完整平台。
3. **惯性只有拖拽有**——zoom / rotate / tilt 松手即停。Cesium 三者都有 `inertiaZoom/Spin/Translate`。缺了缩放甩动的"顺滑收尾"。
4. **手势是模态互斥的**（zoom XOR pan XOR tilt，靠阈值门控）——成熟引擎从双指相似变换里**同时**解算缩放+旋转+平移。本引擎的门控在临界值会"粘滞/跳挡"。
5. **无相机飞行**——双击缩放是**瞬跳**(`distance*0.57`)，programmatic 只有 `viewDistance` 硬切，没有带缓动的 `flyTo`。

下面逐维度展开。文中"Cesium/Maps 如此"的机制我基于对这些引擎架构的了解，`.ref/` 内只有 cesium-native 的瓦片选择库、没有相机控制器源码，故相机对照未逐行核对源码——已在每处标注。

---

## 1. 现状架构（Android 基准，已逐行核对源码）

```
Android UI 线程                    渲染线程(C++)
┌──────────────────┐   JNI   ┌────────────────────────────────────────┐
│ GLESView.onTouch │ ──────▶ │ GLESView.cpp: native* → InputEvent      │
│ 手搓多点状态机    │         │   → gRenderThread.post()  (串行化)       │
│ (无 GestureDetect)│         └───────────────┬────────────────────────┘
└──────────────────┘                         ▼
                              InputManager.process()   ← 手势状态机
                              (Drag/Click/DoubleClick/Pinch)
                                             ▼ callback(Gesture, InputEvent)
                              SceneInputCoordinator.handleGesture()
                                             ▼
                              CameraController  ← 锚点球面相机 + 惯性
```

关键文件：
- `scaffold/examples/android/earthsdk/.../GLESView.java:77` — `onTouchEvent`，**手写**双指跟踪（`pinchPointerId0/1`），不用 `ScaleGestureDetector`/`VelocityTracker`。
- `scaffold/examples/android/MinimalGlobe/GLESView.cpp:447-571` — JNI → `InputEvent` → 投递渲染线程。
- `scaffold/src/earth_engine/interaction/InputEvent.h` — 归一化事件（8 种 Type，含双指原始坐标对 `pointer0/1`）。
- `scaffold/src/earth_engine/interaction/InputManager.cpp` — 状态机：`Idle/OneFingerPending/OneFingerDrag/TwoFinger`；识别 Drag、Click、DoubleClick(0.35s)；Pinch 直通。
- `scaffold/src/earth_engine/camera/CameraController.cpp` — 相机全部数学。
- `scaffold/src/earth_engine/scene/SceneInputCoordinator.cpp:56-101` — Gesture→相机映射。

**相机模型**：`rotation_`(dquat) + `distance_`(地球半径倍数) 的 orbit 表示，交互时切 `orbitMode_=false` 直接改 `Camera` 的 eye/dir/up。即"轨迹球地球"模型——比 Cesium 的完整 6DOF 自由相机简单。

**已确认的优点**（避免被下面的缺口列表误导）：
- 单指拖拽 = 锚点法（抓地表点→旋转相机让该点跟手），与 Cesium `pan3D` 同思路，正确。
- 双精度四元数；地形高度碰撞带 9km 快路径跳过（`clampEyeToMinAltitude`），是拖动流畅的关键。
- 速度惯性（`v*=exp(-3t)`, cap 5rad/s, EMA 0.35）——已修复历史 `touchInertia` 的 36× 发散 bug，鲁棒。
- 双击缩放**已接线**(`SceneInputCoordinator.cpp:91`)；iOS/Android 都走同一 C++ 核心。

---

## 2. 逐维度差距

### 2.1 Pan / 拖拽 — P0：地平线死区
- **现状**：`applyAnchorDrag`(`CameraController.cpp:554`) 每次 move 重新 `pickSurfacePoint`；**打空则 `return`**（line 561-563），相机不动。
- **成熟做法**：Cesium `_pan3D` 当起点或终点 ray 未命中椭球时切 `_spin3D`——用屏幕像素位移 × 与到球心距离相关的系数做**恒速旋转**，保证仰视/近地平线/太空视角下拖拽永远有响应。
- **影响**：低空大倾角、或缩到很远看到地球边缘时，边缘区域拖拽失灵，手感"卡住"。**最该补的一条。**
- 附带：抓取 fallback `intersectGrabSphere`(line 503) 用**球**(`grabbedRadius`)而非 WGS84 椭球——但 `surfacePicker` 已注入真椭球/地形 pick，球面仅兜底，影响很小。

### 2.2 Zoom — 惯性缺失 + jerk 钳制偏保守
- **现状**：pinch 沿 `camera.direction()` 移动 `distanceToAnchor*(scale-1)`，再 `keepAnchorAtScreenPoint` 把锚点转回指下 → 复合成 zoom-to-cursor，思路对。每事件 `scale` 钳到 `[0.7,1.3]`(`kTouchJerkLimit`)。
- **差距**：
  - **无缩放惯性**：`onPinchEnd` 直接清零。Cesium `inertiaZoom` 让快速捏合松手后继续滑一小段。
  - **jerk 钳制 per-event** 在快速捏合时会截断，感觉"追不上手指"。成熟引擎按时间累积而非单事件硬钳。
  - **无桌面滚轮 zoom-to-cursor**（macOS 未接输入，见 §2.7）。
  - 无 `minimumZoomDistance/maximumZoomDistance` 的独立语义（这里用 `kMinAltitudeMeters=50` / `kMaxDistanceEarthRadii=30` 混在碰撞里）。

### 2.3 Rotate / Tilt (heading/pitch/roll) — 非一等公民
- **现状**：双指旋转绕锚点 earth-up 轴(`rotateCameraAroundPoint`)；倾斜绕 camera-right 轴带 `minSlope=0.1` 约束(`rotateCameraVerticalAroundPoint`)。均无惯性。
- **差距**：
  - `Camera` 无 heading/pitch/roll 一等接口，也无 `setView(heading,pitch,roll)`；朝向是 lookAt 隐式结果。**无 north-up 复位 / 指北针 / roll**。Cesium `camera.setView({orientation:{heading,pitch,roll}})`、`constrainedAxis` 北向锁都缺。
  - 倾斜 pivot 是启动时锚点 + 启发式 `minSlope`，不像 Cesium `tilt3D` 那样随地形自适应 pivot、也没有防止倾进地下的"相机被地形顶起"碰撞（这里只有 eye 的 50m floor 钳制，不处理视线穿地）。
  - 旋转/倾斜**无惯性**。

### 2.4 惯性 / 动量 — 只覆盖 1/4 交互
- 拖拽有速度惯性；**zoom / rotate / tilt / 双指 pan 全无**。Cesium `inertiaSpin/Zoom/Translate` 默认都开。缺的是"松手顺滑收尾"的高级感。

### 2.5 双指手势解算 — 模态互斥 vs 联合变换
- **现状**：`onPinchGesture`(line 228-260) 用阈值把意图**分类**为 zoom / pan / tilt 之一（`scaleDominant`、`tiltIntent`、`centerIntent` 互斥门控），临界处会"跳挡/粘滞"。
- **成熟做法**：从两指的 (起点对→终点对) 解一个**相似变换**(平移+旋转+均匀缩放)，缩放/旋转/平移**同时连续**施加，无模态。这是 Google Maps/Mapbox 手感顺滑的根源。本引擎已经把 `pointer0/1` 原始坐标传进来了（`InputEvent.hasPointerPair`），**具备升级到联合解算的数据基础**，但当前没用。

### 2.6 输入管线鲁棒性 — Android 侧手搓的坑
- **只跟 2 指**：`GLESView.java` 手写 `pinchPointerId0/1`，第 3 指按下/指序变更处理脆弱。
- **没用 `VelocityTracker`**：自己在 C++ 里用事件时间戳重算速度——Android 系统的 VelocityTracker 有更好的预测与去抖。
- **没用历史采样**：`MotionEvent.getHistoricalX/Y`（120/240Hz 触摸时一帧内有多个采样点）被丢弃，高速拖动精度下降、惯性速度估计偏噪。
- **没用 `ScaleGestureDetector`/`GestureDetector`**：等于重造轮子，且少了系统的 fling、long-press、tap-slop 校准。
- 优点：渲染线程串行消费输入，无数据竞争（设计干净）。

### 2.7 平台对等性 — 严重不齐
| 能力 | Android | iOS | macOS |
|---|---|---|---|
| 单指拖拽 | ✅ | ✅ | ❌ |
| 捏合缩放 | ✅ | ✅ | ❌ |
| 双指旋转 | ✅ | ❌ 未接 `UIRotationGestureRecognizer` | ❌ |
| 双指倾斜 | ✅ | ❌ | ❌ |
| 鼠标滚轮/右键拖拽/键盘 | — | — | ❌ `MetalView.mm` 无任何输入处理 |

- iOS：`MetalView.mm:153` 只加了 `UIPan`+`UIPinch`，**没有旋转手势**，`rotationRadians` 恒 0 → iOS 用户无法旋转/倾斜。
- macOS：`grep` 确认 `MetalView.mm` 内**零** `mouseDragged/scrollWheel/magnify`，纯展示窗口。桌面完全不可交互。

### 2.8 缺失的交互 / 功能
- **无带缓动的相机飞行**：双击是瞬跳(`viewDistance`, `SceneInputCoordinator.cpp:91`)，`flyTo`/`easeTo` 全无。成熟引擎双击/复位都是动画过渡。
- **无 2D / Columbus 视图模式**及模式间 morph（Cesium 三模式）——本引擎仅 3D 球。
- **无两指点击缩小 / 单指双击拖拽快速缩放**(Google Maps 手势集)。
- **无极点纬度钳制**：orbit 四元数可转到极点附近产生扭转（`constrainedAxis` 缺）。
- **无手势与系统边缘手势的冲突处理 / 指针捕获**。

---

## 3. 优先级建议 —— Android 优先（AI 执行工时基准）

> 已按用户指示收敛到 **Android**；iOS/macOS 平台对等项(§2.7)移出主线，见 §3.1 挂起。
> 排序依据："观感/正确性影响 × 是否卡其它体验"，非人手工时。

| # | 事项 | 类别 | 粗估(AI协作) |
|---|---|---|---|
| A0 | ✅ **已落地** 拖拽地平线 `spin3D` 回退（消死区）—— `onDragStart` miss 不再放弃拖拽、`applyAnchorDrag` 打空 latch 到转台旋转并共用惯性通道；新增 2 回归测试(`DragSpinsWhenPointerMissesGlobeInsteadOfDeadZone` / `DragSpinFlickOverEmptySpaceSeedsInertia`)，native 146/146 + camera 27/27 | 观感/正确性 | 半天 |
| A1 | ✅ **已落地（收窄）** 双指缩放/旋转锚点锁 —— 深读后修订 §2.5：组合缩放+旋转的锚点锁本已正确，"联合"缺口远小于审计所述；用户拍板**保留"双指不平移"既有模型**（不推翻 `PinchHorizontalPanDoesNotMoveCamera`）。实修=消除 `kPinchRotateThresholdRadians` 0.003→0.0003 旋转死区（慢速拧动不再被吞、逐帧响应且锁锚点）。新增 2 不变量测试，146/146 + camera 29/29 | 观感 | 收窄后 ~2h |
| A2 | zoom / rotate 惯性（复用现有 `inertiaAxis_/inertiaAngularVelocity_` 通道 + 新增缩放惯性标量） | 观感 | 半天 |
| A3 | Android 输入健壮性：接 `VelocityTracker` + 消费 `getHistoricalX/Y` 一帧多采样 | 鲁棒/精度 | 半天 |
| A4 | 带缓动 `flyTo`（双击/复位/programmatic 统一走它，替换 `viewDistance` 瞬跳） | 观感 | 1 天 |
| A5 | heading/pitch/roll 一等接口 + north-up 复位/指北针 | 功能 | 1 天 |
| A6 | 极点纬度钳制、单指双击拖拽快速缩放、双指点击缩小等长尾手势 | 功能 | 视需求 |

### 3.1 挂起（非 Android，后续再管）
- macOS `MetalView` 输入接线（鼠标滚轮/右键拖拽/键盘）——桌面当前纯展示。
- iOS `UIRotationGestureRecognizer`（旋转+倾斜对齐 Android）。
- 2D / Columbus 视图模式与 morph。

**注意**：本文件是调研结论，未改任何代码。相机对照基于对 Cesium/Maps 架构的了解（`.ref/` 只有 cesium-native 瓦片选择库、无相机控制器源码）；落地 A0/A1 前建议先把 CesiumJS `ScreenSpaceCameraController.js` 拉进 `.ref/` 逐行对齐 `_spin3D`/`_pan3D`/pinch 解算。
