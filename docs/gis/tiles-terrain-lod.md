# 瓦片、地形与 LOD 规则

地球引擎的流畅性主要来自正确的瓦片调度和 LOD。AI 不得只按文件名或 zoom 直觉实现瓦片系统，必须明确 tile scheme、bounds、误差模型和缓存策略。

## 瓦片方案

实现或接入瓦片前，必须确认：

- 使用 XYZ、TMS、WMTS TileMatrixSet，还是自定义 quadtree？
- tile id 顺序是 `z/x/y` 还是其他顺序？
- y 轴原点在北还是南？
- 瓦片覆盖范围是 Web Mercator 世界、geographic rectangle，还是局部区域？
- 坐标边界是否包含极区，是否裁剪到 Web Mercator 常用纬度范围？
- 图层使用的逻辑坐标系、瓦片坐标系和服务商坐标系是否一致？
- 是否涉及 GCJ-02、BD-09、百度墨卡托、局部偏移或 provider 私有 tiling scheme？

多瓦片体系叠加必须遵守 `multi-tile-schemes.md`。不要把不同 provider 的瓦片都假设为标准 Web Mercator。

## LOD 选择

LOD 不能只靠“相机近就加载高 zoom”这种口头规则。应选择明确指标：

- 2D/影像瓦片可使用 zoom level、屏幕像素覆盖、分辨率。
- 3D terrain 可使用 screen-space error、geometric error、tile bounding volume。
- 3D Tiles 应遵守 tileset 中的 `geometricError`、`refine`、bounding volume 和 transform。

LOD 选择需要防抖和滞回，避免相机轻微移动造成瓦片频繁替换。

## 地形瓦片

地形实现必须说明：

- 高程基准：ellipsoid height、orthometric height，还是数据源自定义高度。
- 高程单位：通常为 meter。
- 网格格式：regular grid、quantized mesh、heightmap，还是自定义 mesh。
- 法线来源：数据自带、CPU 计算、GPU 计算，还是 shader 估算。
- tile 边缘是否使用 skirt 或边缘对齐策略来避免裂缝。
- 不同 LOD 邻接时如何处理 T-junction 和接缝。

## 影像图层

影像图层必须记录：

- URL 模板或 provider 类型。
- CRS / tile matrix set。
- 透明度、叠加顺序、色彩空间和纹理格式。
- 失败瓦片的重试、占位、降级策略。
- 版权和 attribution 要求。

作为地球底图的影像/地图瓦片，其每帧可见性选择、请求调度、缓存、纹理上传、父子替换、多图层混合和失败降级必须遵守 `basemap-tile-rendering.md`。

## 瓦片调度

调度器应具备：

- 请求优先级：屏幕中心、相机近处、当前可见区域优先。
- 并发限制：避免把浏览器连接池和服务器打满。
- 取消机制：相机移动后取消不可见或低优先级请求。
- 缓存策略：内存缓存、磁盘缓存或浏览器缓存的职责边界。
- 父子瓦片渐进替换：子瓦片未完成时可显示父瓦片。

## 验证方式

至少需要：

- tile id 到 bounds 的单元测试。
- bounds 到 tile id 的反向测试。
- zoom/LOD 边界测试。
- y 轴方向测试。
- 反经线或世界边界测试。
- 可视化 debug overlay：tile 边界、z/x/y、加载状态、LOD error。
