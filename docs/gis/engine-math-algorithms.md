# 地球引擎数学与算法边界

本文件规定哪些数学可以手写，哪些必须依赖成熟库或明确测试。地球引擎中的数学错误通常不会报错，只会在高纬度、远距离、近地视角或大数据量时暴露。

## 可以手写但必须测试的内容

- 经纬度 degree/radian 转换。
- WGS84 椭球参数封装。
- Cartographic 到 ECEF 的基础转换。
- ECEF 到局部 ENU 的旋转矩阵。
- Ray 与 sphere/ellipsoid 的基础交点。
- Frustum plane 提取。
- AABB/sphere 与 frustum 的保守相交测试。
- tile z/x/y 与 bounds 的互转。
- 屏幕坐标到 picking ray。

这些函数必须有单元测试，不能只靠可视化。

## 优先使用成熟库的内容

- 任意 CRS 转换。
- datum shift。
- 复杂 polygon overlay。
- topology repair。
- 大地线距离、面积、buffer。
- 复杂投影。
- 高精度地形重采样。
- 复杂 mesh simplification。

## 数值稳定性

必须避免：

- 用 `float32` 保存权威世界坐标。
- shader 中对两个巨大且接近的世界坐标直接相减。
- near plane 过小、far plane 过大导致 depth precision 崩溃。
- 在高纬度用近似公式代替明确的地理计算。
- 在反经线附近用简单 min/max 经度判断范围。

## Picking 算法

拾取应按场景需求选择：

- 椭球拾取：ray-ellipsoid intersection。
- 地形拾取：ray-terrain intersection，可用深度或逐级细化。
- 对象拾取：GPU color/id picking 或 BVH/ray casting。
- 3D Tiles feature picking：需要 feature id 和 metadata。

拾取结果必须说明：

- 返回点是否贴地形。
- 高度来源是什么。
- 坐标是 cartographic、ECEF、ENU，还是 screen。
- 没有命中时如何表示。

## Culling 算法

Culling 应优先保守，宁可多渲染一点，也不要错误剔除可见内容。

常见层级：

- frustum culling
- horizon culling
- backface culling
- occlusion culling
- screen-space error refinement

每种 culling 都需要 debug 开关，以便排查瓦片消失、闪烁或错裁。

## LOD 与误差

LOD 选择应基于可解释误差：

- 影像：屏幕像素分辨率与瓦片原始分辨率。
- 地形：几何误差、屏幕空间误差、相机距离。
- 3D Tiles：tileset geometricError 和 screen-space error。

不要用 magic number 作为唯一依据。必须把阈值命名并记录单位。
