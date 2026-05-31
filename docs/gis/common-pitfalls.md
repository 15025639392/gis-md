# GIS 常见坑

阅读或修改空间代码时，用这个清单辅助检查。

- 把 `[longitude, latitude]` 和 `[latitude, longitude]` 写反
- 混用 `EPSG:4326` 和 `EPSG:3857`，但没有显式坐标转换
- 把 degree 当成 meter
- 因为 Web Mercator 方便渲染，就拿它计算真实距离或面积
- 对全球经纬度数据直接使用平面几何操作
- 忘记不同库对 CRS axis order 的暴露方式可能不同
- 假设所有 bounding box 都使用同一种顺序
- 忽略非法 polygon、未闭合 ring、自相交、洞、多面
- 丢失 Z/M 坐标但没有记录
- 简化 geometry 时没有在需要拓扑一致性的场景下保拓扑
- 忽略反经线和极区边界情况
- 空间查询没有确认索引是否生效
- 假设所有 SDK 或 provider 的 tile z/x/y 顺序都一样
- 把展示精度和存储/分析精度混为一谈
