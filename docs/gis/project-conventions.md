# 项目 GIS 约定

这是 `earth-md` 的本地 GIS 规则。只要项目做出了明确的 GIS 设计选择，就应该更新本文件。

## 默认坐标模型

- 默认地理坐标系：`EPSG:4326`，除非某个功能明确声明使用其他 CRS。
- 默认 GeoJSON 坐标顺序：`[longitude, latitude]`，代码中可简写为 `[lng, lat]`。
- 面向用户的界面文字可以写 “lat/lng”，但内部 GeoJSON 数组必须保持 `[lng, lat]`。
- 如果数值单位不能从上下文明确看出，必须显式记录单位。

## CRS 与投影规则

- 在模块边界、持久化数据边界、API 边界和测试 fixtures 中，CRS 必须明确。
- 地图渲染可以在地图 SDK 或瓦片系统需要时使用 Web Mercator，也就是 `EPSG:3857`。
- 不得把 Web Mercator 坐标直接用于权威距离、面积或缓冲区计算，除非这个限制已经被明确接受，并针对目标尺度做过测试。
- 坐标转换必须使用成熟 CRS 库，例如 PROJ、GDAL、proj4js 或项目认可的等价方案。

## 几何与数据格式规则

- GeoJSON 必须遵守 RFC 7946，除非某个模块明确说明它使用的是内部非 GeoJSON 格式。
- 在昂贵的拓扑操作或持久化之前，应检查几何合法性。
- Bounding box 必须说明坐标顺序和 CRS。
- Polygon ring 应该闭合。不要假设 ring orientation，除非所选库或标准对该操作有明确要求。

## 空间数据库规则

- 如果使用 PostGIS，必须有意识地选择 `geometry` 或 `geography`：
  - `geometry` 适合投影平面操作和局部几何工作流。
  - `geography` 适合经纬度数据上的测地线距离/面积工作流，前提是 PostGIS 的行为符合产品需求。
- CRS 转换使用 `ST_Transform`，不要手写坐标转换公式。
- 大规模空间查询必须确认空间索引是否生效。

## 测试规则

GIS 测试至少应覆盖以下一种明确断言：

- 坐标顺序
- CRS 或投影假设
- 单位
- 与该操作相关的边界行为

优先使用小而明确、预期结果已知的 fixtures，不要依赖大型且不透明的数据 dump。
