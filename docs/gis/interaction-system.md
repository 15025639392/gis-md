# 地球引擎交互系统

地球引擎交互不是普通 view 点击。每个输入事件都可能对应屏幕坐标、射线、椭球点、地形点、3D Tiles feature、业务对象、时间状态和图层状态。AI 设计交互时必须先定义交互契约，而不是在 UI 组件里临时拼逻辑。移动端输入源主要为触控（单指/双指/三指手势）和触控笔，需通过平台桥接层归一化为统一 InputEvent。

## 交互模块边界

推荐拆分：

- `InputManager`：统一 mouse、touch、pointer、wheel、keyboard、gamepad。
- `GestureRecognizer`：识别 click、double click、drag、pinch、rotate、long press。
- `CameraController`：orbit、pan、zoom、tilt、fly-to、follow、collision。
- `PickingService`：screen position 到 scene hit / feature hit / cartographic。
- `SelectionManager`：hover、selected、multi-select、highlight。
- `EditingManager`：绘制、编辑、吸附、撤销/重做。
- `MeasurementTool`：距离、面积、高程、剖面、方位角。
- `LayerInteraction`：图层可见性、透明度、顺序、查询、过滤。
- `TimelineController`：时间轴、播放、暂停、速度、时间窗口。
- `CommandBus`：把 UI 命令和引擎状态修改分离。

## 输入事件

所有输入事件应归一化为统一结构：

```text
InputEvent {
  type: "pointer-down" | "pointer-move" | "pointer-up" | "wheel" | "key"
  screen: [x, y]
  devicePixelRatio: number
  pointerType: "mouse" | "touch" | "pen"
  buttons?: number
  modifiers?: { shift: boolean, ctrl: boolean, alt: boolean, meta: boolean }
  timestamp: number
}
```

不要让业务工具直接读取原始平台事件（iOS UITouch/UIEvent、Android MotionEvent），否则屏幕密度、渲染 surface 尺寸和 view 布局差异很容易出错。所有输入应通过 `InputManager` 和 `GestureRecognizer` 归一化为引擎内部 `InputEvent`。

## 相机交互

相机控制必须明确：

- 左键/单指拖动：旋转、平移，还是按模式切换。
- 右键/双指：倾斜、旋转或平移。
- 滚轮/双指缩放：缩放中心是屏幕中心还是鼠标位置。
- 双击：放大、定位、选中，还是禁用。
- fly-to：持续时间、缓动、最大高度、是否避开地下。
- follow：跟随动态对象时的 heading、pitch、range。
- collision：是否阻止穿地、穿模型、进入地下。

相机状态应可序列化：

```text
CameraState {
  position: Cartographic | ECEF
  heading: number
  pitch: number
  roll: number
  range?: number
  target?: Cartographic | ECEF
}
```

### 地球拖拽旋转：Arcball / Virtual Trackball

单指拖动地球时，默认优先采用 Arcball / virtual trackball 模型，而不是简单的 `yaw/pitch` 欧拉角累加。原因：

- 欧拉角 `pitch` 通常需要 clamp，接近极区时会出现“顶住”“翻不过去”或方向突变。
- Arcball 将屏幕拖动映射为虚拟球面上的两个点，通过任意轴四元数旋转累计姿态，可以自然越过极区。
- 四元数累计旋转更适合后续加入惯性、双指旋转和飞行动画插值。

交互契约：

- 输入：拖动起点/终点屏幕坐标，单位为渲染 surface 像素；必须明确是否已包含 devicePixelRatio。
- 状态：地球姿态或 orbit target 姿态保存为单位四元数，不保存为无限增长的欧拉角。
- 输出：更新后的相机 orbit 姿态或 globe model 姿态。正式引擎应优先旋转相机/target frame；demo 可临时旋转 globe mesh。
- 跟手性：任意屏幕方向的拖拽都必须表现为自然跟手的地球旋转，手指或鼠标位移方向不能在赤道、极区、屏幕边缘或跨越极点时突然反转。
- 极区：不得通过简单禁止 `pitch` 越界来处理极区拖拽。若需要限制视角，应在 CameraController 层用明确的 collision/tilt 约束实现。
- 极区稳定性：连续拖拽经过北极或南极附近时，旋转方向必须连续，不能出现抖动、翻转、卡住、反向加速或突然重置 heading/roll。
- 输入打断：新的 pointer-down、pinch、fly-to 或程序化相机切换必须能中断当前惯性。

屏幕拖拽映射必须跟随当前缩放级别。Arcball 半径不能固定取屏幕短边，否则双指缩放后同样的手指位移会产生不一致的角速度。推荐用当前相机距离和垂直 FOV 估算地球投影半径：

```text
focalLengthPx = viewportHeightPx / (2 * tan(verticalFov / 2))
projectedGlobeRadiusPx = focalLengthPx * globeRadiusWorld / cameraDistanceWorld
```

当前 demo 使用单位化 WGS84 椭球（赤道半径为 1），因此 `globeRadiusWorld = 1`。正式 ECEF 地球实现必须改用相同世界单位下的相机距离和椭球半径，并记录单位。

### 拖拽惯性

拖拽物理惯性属于 CameraController 行为，不应散落在平台 UI 事件里。推荐模型：

- 拖动时根据相邻 arcball 旋转的 `angle / dt` 估算角速度，单位 rad/s。
- 记录最近旋转轴，单位向量。
- pointer-up 后每帧按 `angle = angularVelocity * frameDt` 继续应用四元数旋转。
- 每帧使用指数阻尼衰减角速度：

```text
angularVelocity *= exp(-dampingPerSecond * frameDt)
```

约束：

- 设置最大惯性角速度，避免异常 MotionEvent 间隔导致飞转。
- `dt <= 0` 或过大时不更新速度样本。
- pointer-down、pinch、程序化相机切换、场景销毁时清零惯性。
- 惯性参数必须命名，例如 `maxInertiaAngularVelocityRadPerSec`、`inertiaDampingPerSecond`，不能只写 magic number。
- 低帧率下必须使用真实 `frameDt`，不能按固定 60 FPS 推进。

## Picking 与命中结果

Picking 必须返回结构化结果：

```text
PickResult {
  screen: [x, y]
  hitType: "none" | "ellipsoid" | "terrain" | "feature" | "model" | "3d-tile" | "label"
  cartographic?: { lng: number, lat: number, height: number, crs: string }
  worldPosition?: [x, y, z]
  layerId?: string
  featureId?: string
  objectId?: string
  metadata?: object
}
```

必须明确优先级：

- label 是否优先于点图标。
- 3D Tiles feature 是否优先于地形。
- 透明对象是否可拾取。
- 被遮挡对象是否可拾取。
- 环境效果如云、雨、雾是否参与拾取。

## Hover 与选择

状态必须集中管理：

- `hovered`
- `selected`
- `multiSelected`
- `highlighted`
- `editing`
- `disabled`

不要让每个图层自己维护一套互相冲突的选中状态。样式表现遵守 `overlay-styling.md`。

多选规则必须明确：

- shift 加选。
- ctrl/meta 切换选择。
- 框选。
- 按图层限制选择。
- 空白点击是否清空。

## 绘制与编辑

绘制工具应有状态机：

- `idle`
- `drawing`
- `editing`
- `dragging-vertex`
- `dragging-feature`
- `snapping`
- `committing`
- `cancelled`

支持对象：

- 点
- 线
- 面
- 圆/缓冲区
- 矩形/bbox
- 路径/轨迹
- 标注
- 3D 模型放置

必须明确：

- 坐标是否贴地形。
- 是否允许跨反经线。
- polygon 是否自动闭合。
- 是否支持 holes。
- 顶点拖动是否重新采样地形。
- 编辑中草图和已保存 feature 的 schema 是否不同。
- 保存前是否做几何合法性验证。

## 吸附 Snapping

吸附能力必须声明：

- 吸附对象：顶点、边、网格、道路、地形、模型表面。
- 吸附距离单位：screen px 还是 meter。
- 吸附优先级。
- 是否跨图层吸附。
- 是否显示吸附提示。

吸附不能改变原始数据 CRS，应该只影响编辑结果生成过程。

## 撤销与重做

编辑、绘制、样式调整、图层操作都应尽量通过 command 记录：

```text
Command {
  id: string
  label: string
  do()
  undo()
  redo()
}
```

不要只存 UI 状态快照。空间编辑应记录可逆操作，例如新增顶点、移动顶点、删除 feature、修改属性。

## 测量工具

测量必须先声明模型：

- 距离：planar、spherical、geodesic，或贴地形路径。
- 面积：投影平面、geodesic area，或地形表面积。
- 高度：ellipsoid height、orthometric height，还是地形相对高度。
- 方位角：真北、网格北，还是屏幕方向。
- 剖面：采样间距、地形数据源、插值方式。

测量结果必须显示单位和模型说明。不能用屏幕距离或 Web Mercator 直接冒充真实距离。

## 时间轴交互

时间轴影响：

- 动态对象。
- 时序影像/气象/海洋数据。
- 太阳、月亮、星空、天气。
- 历史轨迹。

必须明确：

- play / pause / step / speed。
- 时间窗口和缓存窗口。
- 数据缺失时的表现。
- 插值方式。
- UI 显示时区。
- simulationTime 和 dataTime 的关系。

## 图层控制

图层交互包括：

- 显示/隐藏。
- 透明度。
- 顺序。
- 样式切换。
- filter/query。
- solo/mute。
- 图例 legend。
- attribution 显示。

图层控制不应绕过 Provider、Renderer 和 SelectionManager 的生命周期。隐藏图层后应停止不必要请求，或明确保留缓存。

## 查询与信息面板

点击查询必须区分：

- 空间拾取结果。
- 业务属性查询。
- 服务端 identify/query。
- 统计分析。
- 元数据查看。

信息面板应显示数据来源、时间、坐标 CRS 和必要单位，避免用户误解。

## 移动端手势

移动端必须明确：

- 单指拖动。
- 双指缩放。
- 双指旋转。
- 双指倾斜。
- 长按拾取或打开菜单。
- 手势与页面滚动的冲突。
- 小屏控件避让。

触摸目标应足够大，避免把精细 GIS 操作设计成只能用鼠标完成。

## 键盘与可访问性

至少考虑：

- 键盘缩放、旋转、平移。
- Esc 取消当前工具。
- Enter 完成绘制。
- Delete 删除选中对象。
- Tab 在控件间移动。
- 对屏幕阅读器暴露当前工具、选中对象和坐标文本。

高频 GIS 工具可以有快捷键，但不能只有快捷键。

## 错误与取消

每个交互工具都要定义：

- 如何取消。
- 如何恢复。
- 如何处理数据加载失败。
- 如何处理拾取为空。
- 如何处理权限不足。
- 如何处理几何非法。

取消交互不应留下半透明残影、临时 feature、未释放监听器或过期 selection。

## 验收清单

实现交互功能后至少验证：

- 鼠标、触摸板、触摸屏基本可用。
- 屏幕密度不同（@1x/@2x/@3x、Android density）或 view 缩放后 picking 仍准确。
- 相机不会穿地或异常翻转。
- hover/selected/editing 状态一致。
- 绘制和编辑支持撤销/重做。
- 测量结果显示单位和计算模型。
- 时间轴驱动环境和动态数据同步变化。
- 图层隐藏后 picking 和请求行为正确。
- Esc 或取消按钮能清理临时状态。
- 移动端不会被页面滚动或控件遮挡破坏操作。
