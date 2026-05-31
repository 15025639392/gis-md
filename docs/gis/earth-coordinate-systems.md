# 地球坐标系统规则

地球引擎最容易出错的地方不是 shader，而是坐标系统。AI 在写任何坐标、相机、瓦片、地形、模型相关代码前，必须先确定本文件中的概念。

## 常用坐标

- `Cartographic`：经度、纬度、高度，通常为 longitude、latitude、height。
- `Geographic`：经纬度坐标，常见 CRS 为 `EPSG:4326`。
- `ECEF`：Earth-Centered, Earth-Fixed，地心地固三维笛卡尔坐标，单位通常为米。
- `ENU`：East-North-Up，以某个地表点为原点的局部坐标系。
- `Web Mercator`：`EPSG:3857`，主要用于 Web 地图瓦片和渲染，不适合直接做真实面积。
- `Clip Space / NDC`：GPU 渲染管线中的裁剪空间和归一化设备坐标。

## 项目默认

- 默认参考椭球体：WGS84 ellipsoid。
- 默认地理坐标：经纬度以 degree 表示，高度以 meter 表示。
- 默认内部地球三维计算：优先使用 ECEF 或局部 ENU，避免把经纬度直接当 3D 坐标。
- 渲染层可以使用相机相对坐标或局部原点重定位来解决浮点精度问题。

## 精度策略

浏览器 GPU 常用 `float32`，而 ECEF 坐标量级约为地球半径数百万米。直接把大 ECEF 坐标送入 shader，远离原点时会出现抖动。

常见策略：

- CPU 使用 double precision 保存权威世界坐标。
- GPU 使用相机相对坐标，也就是把顶点减去 camera/world origin 后再上传或在 shader 中 high/low split。
- 大场景使用 origin rebasing 或局部 ENU。
- shader 中避免对两个巨大且接近的数做减法。

## 经纬高到 ECEF

实现经纬高到 ECEF 时，必须明确：

- 输入经纬度单位是 degree 还是 radian。
- 高度单位是否为 meter。
- 使用的椭球参数：semi-major axis、flattening、eccentricity。
- 输出坐标单位是否为 meter。

该转换应有单元测试，至少覆盖：

- 赤道和本初子午线附近
- 北极或高纬度
- 高度为 0 和非 0
- degree/radian 误用能被测试抓住

## 局部 ENU

ENU 适合局部模型、标注、测量辅助和相机附近的渲染精度优化。使用 ENU 时必须记录原点经纬高。不同 ENU 原点下的坐标不能直接相加。

## 反经线和极区

- 经度归一化策略必须统一，例如 `[-180, 180)` 或 `[0, 360)`。
- 跨反经线的 bbox 不能简单假设 `minLng < maxLng`。
- 极区附近的投影、瓦片和相机控制需要单独验证。
