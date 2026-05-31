# 数据叠加层样式设计

地球引擎的数据叠加层样式不是简单的 CSS。它需要同时处理 GIS 数据 schema、几何类型、坐标高度、贴地方式、LOD、交互状态、标注避让、GPU 性能和业务语义。

AI 设计点、线、面、标注、模型、点云或分析结果样式时，必须先明确：数据类型、几何类型、属性字段、坐标/高度、样式来源、交互状态、可见层级和性能预算。

## 样式系统分层

推荐把样式拆成四层：

- `LayerStyle`：图层级默认样式、可见范围、渲染顺序、透明度、混合模式。
- `GeometryStyle`：Point、LineString、Polygon、MultiGeometry、Model、PointCloud 等几何样式。
- `DataDrivenStyle`：基于 feature 属性、metadata、时间、状态的样式表达式。
- `InteractionStyle`：hover、selected、editing、disabled、alert、highlight 等交互状态。

不要把样式写死在 Provider、几何解析或业务组件里。Provider 提供数据，Style 负责表达，Renderer 负责高效绘制。

## 样式对象基本结构

建议样式配置至少包含：

```text
OverlayStyle {
  version: string
  layerId: string
  geometryType: "point" | "line" | "polygon" | "label" | "model" | "point-cloud" | "heatmap" | "raster"
  minZoom?: number
  maxZoom?: number
  minDistance?: number
  maxDistance?: number
  altitudeMode?: "clamp-to-ground" | "relative-to-ground" | "absolute" | "clamp-to-3d-tiles"
  renderOrder?: number
  default: GeometryStyle
  states?: InteractionStyles
  rules?: DataDrivenRule[]
}
```

`altitudeMode` 必须明确。二维地图可以忽略高度，但地球引擎不能默认所有东西都在椭球表面。

## 点样式 Point

点数据可能渲染为 circle、icon、billboard、model、label anchor 或聚合点。

必须明确：

- 坐标来源和高度模式。
- 尺寸单位：screen pixel、meter，还是随距离衰减。
- 图标资源、颜色、透明度、描边。
- 是否始终朝向相机 billboard。
- 是否参与深度测试。
- 是否被地形或 3D Tiles 遮挡。
- 是否支持聚合 cluster。
- picking 返回 feature id 还是业务对象 id。

常见字段：

```text
PointStyle {
  shape: "circle" | "square" | "icon" | "billboard" | "model"
  size: number
  sizeUnit: "px" | "meter"
  color: Color
  strokeColor?: Color
  strokeWidth?: number
  iconUrl?: string
  scaleByDistance?: boolean
  depthTest?: boolean
}
```

## 线样式 LineString / Polyline

线数据常见于道路、轨迹、管线、边界、航线、测量线。

必须明确：

- 线是在地表贴地，还是三维空间线。
- 宽度单位：screen pixel 还是 meter。
- 是否跨反经线。
- 是否按地形采样贴地。
- 是否需要箭头、虚线、流动线、渐变、分段颜色。
- line join 和 line cap 规则。
- 大数据量时是否瓦片化或简化。

常见字段：

```text
LineStyle {
  width: number
  widthUnit: "px" | "meter"
  color: Color
  opacity?: number
  dashArray?: number[]
  cap?: "butt" | "round" | "square"
  join?: "miter" | "round" | "bevel"
  arrow?: boolean
  clampToGround?: boolean
}
```

轨迹样式应额外说明时间方向、速度映射、历史尾迹长度和采样策略。

## 面样式 Polygon

面数据常见于行政区、地块、区域、缓冲区、分析结果、风险区。

必须明确：

- fill、outline 是否分开渲染。
- 面是否贴地形。
- 是否允许 holes 和 multipolygon。
- ring orientation 是否由库处理。
- 透明面是否参与 depth test。
- 面与地形 z-fighting 如何处理。
- 大 polygon 是否需要切分、三角化或瓦片化。

常见字段：

```text
PolygonStyle {
  fillColor: Color
  fillOpacity: number
  outlineColor?: Color
  outlineWidth?: number
  outlineOpacity?: number
  pattern?: "solid" | "hatch" | "dots"
  clampToGround?: boolean
  heightMode?: "ground" | "extruded" | "absolute"
  extrudedHeight?: number
}
```

分析面必须记录单位和阈值，例如洪水深度、风险等级、缓冲距离。

## 标注 Label

标注是最容易破坏体验的叠加层，不能简单全部显示。

必须明确：

- 文本字段和 fallback。
- 字体、字号、颜色、描边或 halo。
- anchor、offset、对齐方式。
- min/max zoom 或距离可见范围。
- label collision / avoidance 策略。
- 优先级字段。
- 是否允许重叠。
- 贴地、贴点、贴线或屏幕固定。

常见字段：

```text
LabelStyle {
  textField: string
  fontSize: number
  color: Color
  haloColor?: Color
  haloWidth?: number
  anchor?: "center" | "top" | "bottom" | "left" | "right"
  offset?: [number, number]
  priority?: string | number
  allowOverlap?: boolean
}
```

标注避让应作为独立模块，不能把避让逻辑硬编码到业务图层。

## 多几何 MultiGeometry

MultiPoint、MultiLineString、MultiPolygon 和 GeometryCollection 必须说明：

- 是否拆成多个 render primitive。
- picking 时返回整体 feature 还是子几何。
- 样式规则是否按整体属性还是子几何属性。
- 大对象是否按 tile 或 spatial index 切分。

## 数据驱动样式

数据驱动样式应使用受控表达式，不要执行任意代码。

表达式可以支持：

- 分类：按 `status`、`type`、`level` 映射颜色或图标。
- 分级：按数值范围映射颜色、宽度、大小。
- 插值：按 zoom、distance、value 连续变化。
- 条件：按属性存在、布尔状态、时间范围切换样式。

示例：

```text
DataDrivenRule {
  when: ["==", ["get", "status"], "warning"]
  style: { color: "#ffcc00", iconUrl: "warning.svg" }
}
```

必须定义字段不存在、类型错误、值越界时的 fallback。

## 交互状态样式

常见状态：

- `normal`
- `hover`
- `selected`
- `highlighted`
- `editing`
- `disabled`
- `hidden`
- `alert`

状态样式不应破坏业务语义。例如风险等级颜色不能在 selected 状态下完全丢失，应使用描边、halo、亮度或附加效果表达选中。

## LOD 与可见性

样式必须有可见性策略：

- 远距离显示聚合或热力。
- 中距离显示简化线面。
- 近距离显示完整几何、标注和交互点。
- 标注按优先级和碰撞结果显示。
- 点图标可按距离缩放或替换为简化符号。

不要在全球视角渲染大量独立 DOM 标注或高复杂度 polygon。

## 高度与贴地

地球引擎必须明确高度模式：

- `clamp-to-ground`：贴地形。
- `relative-to-ground`：相对地形高度。
- `absolute`：绝对椭球高或项目定义高度。
- `clamp-to-3d-tiles`：贴 3D Tiles 表面。

必须说明高度基准和单位。建筑、设备、轨迹、飞行路径、地下管线不能混用同一高度假设。

## 渲染顺序与深度

样式应明确：

- render order。
- depth test 是否开启。
- 透明对象排序策略。
- polygon offset 或防 z-fighting 策略。
- label/billboard 是否总在最上层。
- 地下对象、遮挡对象如何表达。

## 性能规则

- 大量点优先使用 instancing、batch 或 GPU buffer。
- 大量线面优先瓦片化、简化和分层加载。
- 图标使用 atlas，避免每个点单独纹理。
- 样式变化要区分 uniform 更新、attribute 更新和重建 geometry。
- hover/selected 不应导致整层重建。
- 频繁变化的动态对象应与静态图层分开渲染。

## 样式版本与迁移

样式 JSON 必须有 `version`。当字段语义、颜色规则、单位或表达式语言变化时，需要迁移策略。不要让旧样式静默套用到新数据 schema。

## 验收清单

实现叠加层样式后，至少验证：

- 点、线、面在正确位置显示。
- 样式字段缺失时有 fallback。
- hover/selected/editing 状态清晰且不丢失业务语义。
- 远近距离和不同 zoom 下可读。
- 标注没有大面积重叠。
- 贴地对象随地形变化正确。
- 透明面没有明显 z-fighting。
- 大数据量下不会明显卡顿。
- picking 返回正确 feature id。
- 截图或视觉测试覆盖桌面和移动视口。
