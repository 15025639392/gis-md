# 地球引擎交互系统

地球引擎交互不是普通 DOM 点击。每个输入事件都可能对应屏幕坐标、射线、椭球点、地形点、3D Tiles feature、业务对象、时间状态和图层状态。AI 设计交互时必须先定义交互契约，而不是在 UI 组件里临时拼逻辑。

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

不要让业务工具直接读取原始 DOM 事件，否则移动端、DPR、canvas 缩放和嵌入布局很容易出错。

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
- DPR 不同或 canvas 缩放后 picking 仍准确。
- 相机不会穿地或异常翻转。
- hover/selected/editing 状态一致。
- 绘制和编辑支持撤销/重做。
- 测量结果显示单位和计算模型。
- 时间轴驱动环境和动态数据同步变化。
- 图层隐藏后 picking 和请求行为正确。
- Esc 或取消按钮能清理临时状态。
- 移动端不会被页面滚动或控件遮挡破坏操作。
