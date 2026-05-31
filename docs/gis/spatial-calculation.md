# 空间计算规则

当 CRS、单位和几何模型不清楚时，空间计算很容易“看起来能跑，但结果是错的”。写代码前先用本文件选择计算模型。

## 距离

- 经纬度中的 degree 是角度单位，不是米。
- 点到点的地理距离应使用测地线 geodesic，或使用文档明确说明的球面距离函数。
- 局部平面距离应先把数据转换到合适的投影坐标系，并记录所用 CRS。
- 数据库距离查询必须确认函数作用于 `geometry` 还是 `geography`，以及返回单位是什么。

## 面积

- 不得直接对未投影的经纬度坐标使用平面公式计算真实世界面积。
- 局部或区域面积应使用合适的等面积投影，或适合该区域的投影 CRS。
- 全球范围或大范围 polygon 面积，应使用可信库或数据库提供的 geodesic area 能力。
- 必须记录输出单位，默认优先使用平方米，除非产品需求另有规定。

## 缓冲区

- 用平面几何库直接 buffer 经纬度坐标通常是错的，因为 buffer 距离会被解释为 degree。
- 米级 buffer 应使用 `geography` 能力，或先转换到合适的投影 CRS 后再 buffer。
- 如果产品可能覆盖极区、反经线或大范围区域，必须验证这些位置的 buffer 行为。

## 拓扑与谓词

- GEOS/Shapely 风格的谓词是平面操作。必须确认 CRS 适合该操作。
- 对 `contains`、`within`、`intersects` 等谓词，要检查所选库的边界语义。
- 非法 polygon 可能导致意外的谓词结果。数据质量不确定时，应先 validate 或 repair geometry。

## 瓦片与 Web Mercator

- XYZ tiles 通常使用 Web Mercator 和 zoom/x/y 地址。
- Tile 坐标、屏幕像素、投影坐标米值、地理坐标不能互相混用。
- 必须区分 tile z/x/y、pixel coordinates、projected coordinates、geographic coordinates。

## 反经线与极区

跨越 longitude `180/-180` 的情况和极区必须作为显式边界情况处理。如果产品不支持这些区域，应记录限制，并测试拒绝或归一化路径。
