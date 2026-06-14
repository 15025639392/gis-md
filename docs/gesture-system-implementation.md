# 地图引擎手势系统实施说明

本文档用于指导手势系统重构实施。当前项目既有手势实现、OpenGlobus、Cesium 均不作为行为参考；本次实现只围绕明确的产品交互目标设计。

## 目标

实现一套简单、稳定、可测的地图手势系统：

- 单指拖拽：拖动地球/地图视图，松手后有合理物理惯性。
- 双指向上/向下推：倾斜视角。
- 双指旋转：旋转视角。
- 双指缩放：放大/缩小视图。

不追求复杂导航模式，不做 OpenGlobus/Cesium 行为对齐，不引入复杂 north-up/free mode 策略。第一版目标是手感干净、动作不串扰、状态可调试。

## 交互契约

### 单指拖拽

用户用一根手指拖动画面时：

- 手指向右拖，地球/地图应跟随手指方向移动。
- 手指向左、上、下拖，同理。
- 松手后进入惯性滑动。
- 惯性应基于松手前最近一小段运动速度，而不是基于某个固定世界轴角速度。
- 惯性应逐渐衰减并停止，不能长时间漂移。
- 新手势开始时必须立即停止旧惯性。

### 双指倾斜

用户两根手指同时向上推时：

- 视角应变得更倾斜，看到更远的地表。
- 两根手指同时向下拉时，视角应回正。
- 该动作由双指中心点的垂直位移驱动。
- 双指中心明显上下移动时，应优先识别为倾斜，不应误触发缩放或旋转。

### 双指缩放

用户两指距离变化时：

- 两指距离变大：放大。
- 两指距离变小：缩小。
- 缩放中心使用双指中心点。
- 轻微手指抖动不应触发明显缩放。

### 双指旋转

用户两指连线角度变化时：

- 视角应围绕手势中心或屏幕中心旋转。
- 顺/逆时针方向应与双指旋转方向一致。
- 轻微角度抖动不应触发明显旋转。

## 模块边界

建议拆成三层。

### 1. RawInputAdapter

职责：把平台输入归一化为 pointer 流。

输入来源可以是 Android touch event、iOS touch event、mouse、wheel 等。该层只做坐标、时间、pointer id、phase 的归一化，不判断地图语义。

建议输出字段：

```cpp
enum class PointerPhase {
    Down,
    Move,
    Up,
    Cancel
};

enum class PointerDevice {
    Touch,
    Mouse,
    Wheel
};

struct RawPointerEvent {
    PointerPhase phase;
    PointerDevice device;
    int pointerId;
    float x;
    float y;
    double timestampSeconds;
    int viewportWidth;
    int viewportHeight;
};
```

### 2. GestureRecognizer

职责：识别手势，不直接改相机。

它维护活跃 pointer 集合，并输出语义手势事件：

```cpp
enum class GestureType {
    None,
    SingleDragStart,
    SingleDragMove,
    SingleDragEnd,
    SingleDragCancel,
    TwoFingerStart,
    TwoFingerTilt,
    TwoFingerZoom,
    TwoFingerRotate,
    TwoFingerEnd,
    TwoFingerCancel
};

struct GestureEvent {
    GestureType type;
    float centerX;
    float centerY;
    float deltaX;
    float deltaY;
    float velocityX;
    float velocityY;
    float scaleDelta;
    float rotationDeltaRadians;
    double timestampSeconds;
};
```

### 3. CameraGestureController

职责：消费语义手势，更新相机。

建议接口：

```cpp
class CameraGestureController {
public:
    void onSingleDragStart(float x, float y, double time);
    void onSingleDragMove(float dx, float dy, double dt);
    void onSingleDragEnd(float velocityX, float velocityY);

    void onTwoFingerTilt(float centerX, float centerY, float deltaY);
    void onTwoFingerZoom(float centerX, float centerY, float scaleDelta);
    void onTwoFingerRotate(float centerX, float centerY, float rotationDeltaRadians);
    void onGestureCancel();

    void update(double deltaSeconds);
};
```

相机层不得直接消费平台 touch event。

## GestureRecognizer 状态机

推荐状态：

```text
Idle
  -> OneFingerPossible
  -> OneFingerDragging
  -> OneFingerEnded

Idle
  -> TwoFingerPossible
  -> TwoFingerTilt
  -> TwoFingerZoom
  -> TwoFingerRotate
  -> TwoFingerEnded

Any
  -> Cancelled
```

### 单指识别

流程：

1. 第一个 pointer down：进入 `OneFingerPossible`。
2. move 距离小于阈值：保持可能点击状态，不输出 drag。
3. move 距离超过阈值：输出 `SingleDragStart`，进入 `OneFingerDragging`。
4. 后续 move：输出 `SingleDragMove`。
5. pointer up：输出 `SingleDragEnd`，携带最近速度。
6. cancel：输出 `SingleDragCancel`，不触发惯性。

推荐阈值：

```text
singleDragStartThresholdPx = 4 ~ 8
```

### 双指识别

两根手指同时存在时，计算三类变化：

```text
centerDelta = currentCenter - previousCenter
scaleDelta = currentDistance / previousDistance
rotationDelta = currentAngle - previousAngle
```

进入双指后先处于 `TwoFingerPossible`，等待某个意图超过阈值。

推荐阈值：

```text
tiltThresholdPx = 8 ~ 12
zoomThresholdRatio = 0.03 ~ 0.05
rotateThresholdRadians = 0.04 ~ 0.06
```

意图判断：

```text
tiltScore = abs(centerDeltaY) / tiltThresholdPx
zoomScore = abs(scaleDelta - 1.0) / zoomThresholdRatio
rotateScore = abs(rotationDelta) / rotateThresholdRadians
```

选择分数最高且超过 1.0 的动作为当前锁定模式。

模式锁定后：

- `TwoFingerTilt`：只输出 tilt，scale 置为 1，rotation 置为 0。
- `TwoFingerZoom`：只输出 zoom，tilt delta 置为 0，rotation 置为 0。
- `TwoFingerRotate`：只输出 rotate，tilt delta 置为 0，scale 置为 1。

第一版建议不要做复杂组合手势。先保证倾斜、缩放、旋转互不串扰。

### 双指模式切换

第一版可以不允许模式切换，直到双指结束。这样最稳定。

如果后续要允许切换，必须满足：

- 新模式 score 至少大于当前模式 score 的 1.5 倍。
- 新模式连续 2 到 3 帧占优。
- 切换时重置 previous distance/angle/center，避免跳变。

## 单指拖拽惯性

### 速度采样

记录最近若干帧 move delta：

```cpp
struct MoveSample {
    float dx;
    float dy;
    double dt;
};
```

建议只保留最近 80 到 120 ms 的样本。

松手时计算加权平均速度：

```text
velocity = weightedAverage(delta / dt)
```

越新的样本权重越高。

限制最大速度：

```text
maxInertiaVelocityPxPerSec = 2500
```

### 惯性积分

CameraGestureController 内保存屏幕空间惯性速度：

```cpp
float inertiaVelocityX;
float inertiaVelocityY;
bool inertiaActive;
```

每帧：

```text
dragBy(inertiaVelocityX * dt, inertiaVelocityY * dt)
decay = exp(-inertiaDecay * dt)
inertiaVelocity *= decay
```

推荐参数：

```text
inertiaDecay = 4.0 ~ 7.0
stopSpeedPxPerSec = 2.0
```

如果惯性太飘，提高 `inertiaDecay`；如果松手后太死，降低 `inertiaDecay`。

### 新手势打断

任何新的 pointer down、two finger start、cancel，都必须调用：

```cpp
stopInertia();
```

## 相机动作实现建议

### 单指拖拽

第一版可以使用屏幕 delta 转换为相机绕地球中心的旋转：

```text
yawDelta = -dx / viewportWidth * dragYawScale
pitchDelta = -dy / viewportHeight * dragPitchScale
```

推荐初始参数：

```text
dragYawScale = 2.0 * PI
dragPitchScale = PI
```

但要注意：这只是第一版简化实现。后续如果需要更强地表跟手，可改为基于屏幕射线和椭球交点的 anchor drag。

### 双指倾斜

双指中心向上：

```text
tiltDelta = -centerDeltaY / viewportHeight * tiltScale
```

推荐：

```text
tiltScale = PI * 0.8
minPitch = 0 degrees
maxPitch = 75 degrees
```

注意方向：

- `centerDeltaY < 0`：手指上推，应增加倾斜。
- `centerDeltaY > 0`：手指下拉，应减少倾斜。

### 双指缩放

建议使用指数或比例距离变化：

```text
newDistance = oldDistance / scaleDelta
```

并做距离 clamp：

```text
minCameraHeight = 50m
maxCameraDistance = 40,000,000m
```

缩放中心第一版可使用屏幕中心或双指中心。若实现成本允许，应优先用双指中心。

### 双指旋转

绕视线方向或地表法线旋转：

```text
heading += rotationDeltaRadians
```

第一版只需保证方向符合手势方向，且不会影响缩放和倾斜。

## 参数集中配置

不要把参数散落在代码里。建议集中成：

```cpp
struct GestureConfig {
    float singleDragStartThresholdPx = 6.0f;
    float tiltThresholdPx = 10.0f;
    float zoomThresholdRatio = 0.04f;
    float rotateThresholdRadians = 0.05f;

    float dragYawScale = 6.2831853f;
    float dragPitchScale = 3.1415926f;
    float tiltScale = 2.5132741f;

    float inertiaDecay = 5.5f;
    float stopSpeedPxPerSec = 2.0f;
    float maxInertiaVelocityPxPerSec = 2500.0f;

    float minPitchRadians = 0.0f;
    float maxPitchRadians = 1.3089969f; // 75 degrees
};
```

## 调试输出

为了真机调试手感，需要在 debug overlay 或日志里输出：

```text
gestureState
lockedTwoFingerMode
pointerCount
centerDeltaX / centerDeltaY
scaleDelta
rotationDeltaRadians
inertiaVelocityX / inertiaVelocityY
inertiaActive
cameraDistance
cameraPitch
FPS / frameTime
```

性能相关日志应低频输出，例如每 250ms 一次，避免污染主线程。

## 单元测试要求

### GestureRecognizer 测试

必须覆盖：

- 小位移单指不会触发 drag。
- 单指超过阈值触发 `SingleDragStart` 和 `SingleDragMove`。
- 单指松手输出 `SingleDragEnd`，且 velocity 非零。
- cancel 清理状态，不触发惯性。
- 双指小抖动不触发任何模式。
- 双指中心明显上移锁定 `TwoFingerTilt`。
- 双指距离明显变化锁定 `TwoFingerZoom`。
- 双指角度明显变化锁定 `TwoFingerRotate`。
- 锁定 tilt 后，scale/rotation 输出被抑制。
- 锁定 zoom 后，tilt/rotation 输出被抑制。
- 锁定 rotate 后，tilt/scale 输出被抑制。
- 双指结束后下一次单指 up 不应触发 click 或 drag end。

### CameraGestureController 测试

必须覆盖：

- 单指拖拽会改变相机视图。
- 单指松手后 update 会继续改变相机视图。
- 惯性 1 到 2 秒内衰减到停止。
- 新手势开始会停止旧惯性。
- 双指上推增加 pitch。
- 双指下拉减少 pitch。
- pitch 不超过最大倾斜角。
- 双指缩放会改变相机距离。
- 双指旋转会改变 heading/roll 或等价旋转状态。

## 性能要求

手势系统不能在输入事件回调中做重计算。

要求：

- 输入事件回调只更新状态和少量数学量。
- 相机变化统一在每帧 `update(dt)` 中应用。
- 不在 move 事件中触发瓦片遍历、网络请求或 GPU 上传。
- 日志低频输出。
- 每帧手势计算应为 O(1)。

## 第一阶段交付标准

第一阶段只验收以下行为：

- 单指拖拽顺滑。
- 单指松手惯性自然，不长时间漂移。
- 双指上推只倾斜。
- 双指缩放只缩放。
- 双指旋转只旋转。
- 三种双指动作互不串扰。
- cancel/new gesture 能清理状态。
- 真机上不会因为手势处理造成明显掉帧。

不要在第一阶段加入复杂地形拾取、OpenGlobus/Cesium 对齐、极区策略、north-up/free mode、自适应模式切换等功能。

## 实施顺序

1. 新增 `GestureConfig`。
2. 新增或重写 `InputEvent`，补齐 pointer id、device、phase、cancel、双指原始点信息。
3. 实现 `RawInputAdapter` 或平台侧等价转换。
4. 实现 `GestureRecognizer` 和单元测试。
5. 改造 `CameraController`，让它只消费语义手势。
6. 实现单指拖拽和惯性。
7. 实现双指倾斜。
8. 实现双指缩放。
9. 实现双指旋转。
10. 添加 debug overlay/log。
11. Android 真机验证手感和 FPS。

## 禁止事项

- 不要把 OpenGlobus/Cesium 当作手势行为参考。
- 不要在相机控制器里直接解析平台 touch event。
- 不要让双指倾斜、缩放、旋转同时无约束生效。
- 不要在 move 事件里做重型渲染/瓦片/网络逻辑。
- 不要用复杂模式掩盖基本手势不稳定。
- 不要为了性能降低可见地图细节或缩短可见距离。

