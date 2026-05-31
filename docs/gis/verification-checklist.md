# GIS 验证清单

在完成 GIS 分析或代码前，回答下面与任务相关的问题。

## 输入

- 输入格式是什么：GeoJSON、WKT、WKB、raster、tile、数据库行、SDK 对象，还是自定义模型？
- 输入 CRS 是什么？
- 输入坐标顺序是什么？
- 输入单位是什么？
- 是否存在非法值、缺失值、低精度值或数据质量问题？

## 操作

- 当前操作是平面 planar、球面 spherical，还是椭球/测地线 ellipsoidal/geodesic？
- 选择的 CRS 是否适合当前操作？
- 计算或渲染前是否需要坐标转换？
- 所选库对这个操作是否足够权威？
- `contains`、`intersects`、`touches` 等谓词的边界语义是否清楚？

## 输出

- 输出 CRS 是什么？
- 输出坐标顺序是什么？
- 输出单位是什么？
- 精度是否只在展示层或 API 边界做 round？
- 错误、不支持情况或近似计算是否对调用方可见？

## 边界情况

- 反经线 antimeridian
- 极区 polar regions
- 大 polygon 或全球范围
- 非常小的距离或面积
- empty、null、invalid 或混合 geometry type
- multipolygon 和 holes
- Z/M 坐标
- 跨日期变更线的 bounding box 归一化

## 测试

- 是否有能抓住 lat/lng 反转的 fixture？
- 是否有 CRS 或单位断言？
- 是否至少有一个边界情况或非法输入测试？
- 对性能敏感的空间查询，是否检查了索引使用或查询形态？
