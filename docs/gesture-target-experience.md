# 手势预期效果契约 v1（Gesture Target Experience Contract）

- 状态：已确认（用户逐项拍板，2026-08-17）
- 适用范围：gis-md 地球引擎交互手势——触摸（单指 / 双指）、桌面（滚轮 / 键鼠 / 双击）、键盘
- 文档优先级：与 `docs/gesture-system-implementation.md` 冲突时，以本文档为准（旧文档为早期实现草案，见第 7 节）

## 0. 定位与手感基准

- 产品定位：**交互式地球**——空间视角可绕地旋转，近地可拖图；不修纯地球仪。
- 手感基准：**苹果机（iOS）**，目标体验是"像滑动列表一样丝滑"：逐帧速度积分、指数衰减、无跳变、跟手 ≤ 1 帧。
- 核心一句话：**同一根手指，空间像拖球，近地像拖图。**

## 1. 单指拖拽

### 1.1 核心语义

- 单指拖拽只有一个语义：**锚点跟随**（起手抓住地表点，拖拽期间钉在指下）。
- 反馈参照系随相机高度/姿态切换：空间 = 旋转角速度反馈（rad），近地 = 像素位移反馈（px）。
- 切换不引入第二套手势/相机数学：近地平移是旋转的平直极限。

### 1.2 模式切换

- 判据在起手时判定一次，**拖拽中途不切换**（防止画面语义突变）：
  - 近地拖图：相机海拔 < 150,000m **且** pitch ≥ 60°；
  - 否则空间拖球。
- 海拔门限 150km：Cesium `minimumPickingTerrainHeight` WGS84 生产默认（低于该高度进入近地拾取/平移行为）。
- 倾斜门限 60°：MapLibre issue #6111 确认 60° pitch 即触发高倾斜反向滑移；本项目 maxPitch = 75°。
- 过渡带：线性混合区，宽度为真机标定项（无公开引擎发布过渡曲线，Cesium/MapLibre 均为硬切换）。
- 地平线几何约束（硬性）：**锚点必须在地平线以内**（`pixelsToHorizon > 0`）。

### 1.3 近地平移

- 姿态完全锁定；Δpx 等量换算为地表位移。
- 锚点像素误差 ≤ 1px（良态区）。
- 位移/惯性偏移 ≤ **0.75 × 地平线像素距离**（MapLibre PR #6345），越界即停——**绝不反向**。

### 1.4 惯性（iOS 列表手感）

- 模型：逐帧速度积分 `v *= 0.998^(dt_ms)`（≈ exp(-2.0/s)），方向锁定、只衰减。
  - 依据：Apple `UIScrollViewDecelerationRateNormal = 0.998`；Mapbox Maps iOS `panDecelerationFactor` 默认同值，文档明确"每毫秒乘一次速度向量"。
- 速度采样：`v = 0.6·v[-2] + 0.35·v[-1] + 0.05·v[0]`（Flutter `IOSScrollViewFlingVelocityTracker` 对 iOS 速度估计的近似，最近三个相邻样本加权）。
- 触发下限：**100px/s**（Mapbox GL Native iOS 与 Flutter iOS `BouncingScrollPhysics` 双重印证）。
- 停止判据：折合屏幕位移 < **0.5px/帧**（Cesium `maintainInertia`）。
- 空间模式：同一衰减律作用于角速度（rad/s），停止判据统一按屏幕位移折算。
- 硬约束：松手方向锁定，全程**不反转、不回拉**；禁止弹簧/振荡弹道；无边缘回弹。

### 1.5 cancel

- Cancel / 新手势：**立即停、清惯性、锚点作废**。
- 现状疑点：`InputManager::cancelActiveGesture` 在拖拽中合成 DragEnd 且可能残留惯性——真机验证项。

## 2. 双指（直接操纵）

### 2.1 总体

- 四轴**组合手势**：缩放 / 旋转 / 倾斜 / 平移同时生效，不做会话级独占 latch（Apple Maps、Google Earth、Mapbox GL JS 均为组合）。
- 倾斜保留**竖直起手锁**：两指各 ≥ 2px、同向竖移、100ms 内判定（Mapbox `TouchPitchHandler`）；该锁只决定倾斜轴是否启用，不影响其他轴。

### 2.2 各轴映射与阈值（Mapbox 生产值）

| 轴 | 手势 | 阈值 | 输出 |
|---|---|---|---|
| 缩放 | 两指距离变化 | 0.1 log2（≈7.2% 距离变化） | 绕锚点对数缩放 |
| 旋转 | 两指连线转角 | 25px 弧长自适应（角度阈值 = 25/(π·d)·360°） | 绕锚点法线旋转 |
| 倾斜 | 两指同向竖移 | 起手竖直判定 | 0.5°/px（保留质心绝对值映射；真机微调） |
| 平移 | 双指整体移动 | 随动 | 锚点刚性平移 |

- 缩放/旋转绕指下锚点，锚点像素误差 ≤ 1px（与单指一致）。
- 任一手指抬起 → 双指结束，立即停。
- 新手势打断旧惯性（单指/双指互抢）。

### 2.3 惯性范围

- 双指 pan：**无惯性**，松手即停（苹果地图直接操纵语义）。
- zoom：**保留惯性**——对数空间指数逼近锚点（`distance *= exp(-r·dt)`），数学上保证永不越过/反向（Cesium `inertiaZoom=0.8`、Mapbox zoom inertia 证据）。
- tilt / rotate：无惯性。

### 2.4 高空行为

- **去掉"高空回中"**（松手后球心自动回屏幕中心）——与"不回拉"硬约束冲突。
- 改为 zoom 几何验收：拉远到高空时球心应**自然**回到屏幕中心（锚点/视线逻辑正确性的结果，而非事后补偿）。

## 3. 滚轮 / 桌面键鼠 / 双击

### 3.1 滚轮缩放

- 语义：绕光标缩放；方向 = 滚轮方向，**绝不反向**。
- 步长：每格 ≈ e^(0.20)（≈ +0.29 缩放级），保留现状量级（交互式地球缩放跨度大，介于 Mapbox 2D 与 Cesium 激进档之间）。
- 单帧上限：每帧最多 +1 缩放级（Mapbox `maxScalePerFrame = 2` 证据），防快速滚轮爆飞。
- 格间平滑：格与格之间做短平滑（Mapbox 缩放/双击缓动证据），不种 zoom 惯性（避免甩飞）。

### 3.2 桌面键鼠映射（Cesium 系）

| 输入 | 动作 | 证据 |
|---|---|---|
| 左键拖 | 锚点拖拽（空间拖球 / 近地拖图） | 本契约 1.1 |
| 右键拖 | 缩放（质心钉起手处，避免横向拖地） | Cesium `zoomEventTypes = [RIGHT_DRAG, WHEEL, PINCH]` |
| 滚轮 | 缩放 | Cesium / Mapbox |
| 中键拖 | 倾斜 | Cesium `tiltEventTypes = [MIDDLE_DRAG, ...]` |
| Ctrl + 左键拖 | 倾斜 | Cesium `tiltEventTypes = [..., CTRL+LEFT_DRAG]` |
| 单击 | pick + 选中 | 现状 |
| 双击（命中地表） | 平滑缩放 +1 级（0.5× 距离、300ms、绕点击点） | Mapbox `click_zoom.js` |
| 双击（未命中） | 不响应 | 现状（天空/太空无地理语义） |

- 双击**不抑制**单击选中：第一次单击照常派发（延迟派发会牺牲单击响应，与"跟手 ≤ 1 帧"冲突）。

### 3.3 键盘（Mapbox 键位）

- 方向键：平移 100px（空间 = 绕地旋转、近地 = 平移，与单指同语义）；
- `+` / `=`：缩放 +1 级；`-`：缩放 -1 级；Shift 加倍（±2 级）；
- Shift + 方向键：左右旋转 15°、上下倾斜 10°。
- 来源：Mapbox `keyboard.js` 生产默认。

## 4. 硬性边界（所有手势）

- 滑行方向锁定，只衰减、**绝不反转/回拉**。
- 近地高倾斜不得出现 Mapbox 式反向滑移（地平线裁剪兜底）。
- 锚点求解病态区（掠射/球缘）连续退化（保留现有 `anchorExactWeight` 机制）。
- 性能：输入回调 O(1)；惯性逐帧积分与渲染帧同步；跟手 ≤ 1 帧；惯性期不触发瓦片/网络/上传阻塞；真实 dt 积分保证 120Hz 下位移守恒。

## 5. 验收标准（可测）

- 单指：60°+ 高倾斜快速上滑**不反向、不飞远**；锚点像素误差 ≤ 1px（良态区）；滑行距离/时长落在推导区间（1000px/s → 约 500px、约 1.75s）；cancel 立即停。
- 双指：组合动作（边捏边转边斜）无串扰、无跳变；倾斜满倾角约 150px 竖移；zoom 惯性永不越过锚点；任一抬起即停。
- 滚轮/双击：快速滚动单帧不超过 +1 级、方向不反向、格间平滑无跳变；双击 300ms 平滑缩放到 +1 级且命中点保持。
- 键盘：方向/缩放语义与单指一致（空间绕地、近地平移）。
- 性能：惯性全程不掉帧（60/120fps），跟手延迟 ≤ 1 帧。

## 6. 真机标定清单

1. 单指速度上限：Flutter 8000px/s vs Mapbox JS 6000px/s vs 旧文档 2500px/s；
2. 松手减速系数（iOS 松手瞬间是否减速，Flutter 注释提示）；
3. 地平线余量 0.75（MapLibre 由 0.5 调至 0.75 属手感调参）；
4. 起手阈值口径（现状 8px vs Flutter `kTouchSlop` 18px；UIPanGestureRecognizer 无此门限）；
5. 模式切换过渡带宽度；
6. 倾斜增益微调（基准 0.5°/px）。
7. 滚轮每格步长（现状 0.20 ln）与格间平滑时长（基准 300ms）；
8. 双击步长（基准 0.5× = +1 级）。

## 7. 证据来源

| 项 | 来源 |
|---|---|
| 模式切换海拔 150km | Cesium `ScreenSpaceCameraController.minimumPickingTerrainHeight = 150000`（WGS84 默认） |
| 模式切换 pitch 60° | MapLibre issue #6111 |
| 地平线裁剪 0.75 | MapLibre PR #6345 |
| iOS 惯性 0.998/ms | Apple `UIScrollViewDecelerationRateNormal`；Mapbox Maps iOS `GestureOptions.panDecelerationFactor` |
| 速度采样 0.6/0.35/0.05 | Flutter `IOSScrollViewFlingVelocityTracker` |
| 触发 100px/s | Mapbox GL Native iOS；Flutter iOS `BouncingScrollPhysics` |
| 停止 0.5px/帧 | Cesium `maintainInertia` |
| 双指阈值 | Mapbox GL JS `touch_zoom_rotate.js`（zoom 0.1 log2 / rotate 25px 弧长 / pitch 2px·100ms / 0.5°/px） |
| zoom 惯性 | Cesium `inertiaZoom=0.8`；Mapbox `defaultZoomInertiaOptions` |
| 组合手势 | Apple Maps（行为）、Google Earth 支持页、Mapbox GL JS 三 handler 并存 |
| 无回拉 / 无弹簧 | 用户确认 + 本项目契约 |
| 滚轮单帧上限 | Mapbox `scroll_zoom.js`（`maxScalePerFrame = 2`） |
| 双击平滑缩放 | Mapbox `click_zoom.js`（+1 级 / 300ms / 绕点） |
| 桌面键鼠映射 | Cesium `ScreenSpaceCameraController` 默认 `eventTypes` |
| 键盘键位 | Mapbox `keyboard.js`（pan 100px / ±1 级 / 15°·10°） |

## 8. 与既有文档关系

`docs/gesture-system-implementation.md` 为早期实现草案；其中"双指互斥 latch""Android 系惯性（spline 衰减 4~7 / 2500px/s）""高空回中"等与本契约冲突，以本文档为准；后续实现按本文档修订。键盘映射为本契约新增项。
