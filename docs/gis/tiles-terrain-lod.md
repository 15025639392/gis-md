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

### Globe surface LOD 选择器

3D globe 的地表瓦片选择应把 OpenGlobus 的 globe segment 空间语义和 cesium-native 的 tile selection 状态机结合起来：

- OpenGlobus 负责地球分区、极区补丁、地平线/前半球可见性、相机所在 segment 保护和 terrain/segment 语义。
- cesium-native 负责可迁移的选择器规则：screen-space error、geometric error、bounding volume 距离、上一帧 selection state、renderable 判断、父子替换和加载队列优先级。
- 不应把低空 `equal-zoom` 作为正式 LOD 策略。它可以作为诊断开关，用来证明局部清晰块来自可渲染节点层级不均匀，但正式策略应通过 SSE、renderable 状态和父子替换解决。

低空局部清晰块的直接成因通常不是影像 provider 本身给了不同 zoom，而是同一高 zoom 影像被贴到不同大小的 `SurfaceTile` / target 覆盖范围上。中心相机分支被细分到更小 surface，周边仍停在粗 surface 时，即使两者都使用 z18 影像，也会因为屏幕采样密度不同而出现局部清晰块。

正式选择器必须至少具备以下状态与规则：

- `TileSelectionState`：区分 `Rendered`、`Refined`、`Kicked`、`NotVisited`，并保留上一帧原始状态。
- `isRenderable`：tile key 被选中不等于能渲染，必须综合 surface mesh、terrain fallback、imagery attachment 或占位策略。
- `Ancestor Meets SSE`：缩小时，如果父 tile 已满足 SSE 但尚不可渲染，应继续允许上一帧已显示的深层子孙渲染，避免细节突然消失。
- `Kicking`：放大时，如果子孙目标 tile 尚未全部可渲染且上一帧也没有显示过，应继续渲染当前父 tile，同时保留子孙加载任务。
- `loadingDescendantLimit`：等待子孙过多时，应优先加载并显示较粗父 tile，避免长时间空白或一次性细节突现。
- raster/imagery target pixels：影像请求和 fallback 不应只看固定 zoom，应参考承载它的 surface tile 屏幕覆盖、目标像素和 provider 限制。

相邻 LOD 均衡可以作为视觉补充，例如限制邻接 tile 最大层级差，但它不应替代选择器状态机。只靠邻居 equal-zoom 会提高请求和 mesh 数量，并掩盖 renderable / fallback / overlay target pixels 的根因问题。

## 地形瓦片

地形实现必须说明：

- 高程基准：ellipsoid height、orthometric height，还是数据源自定义高度。
- 高程单位：通常为 meter。
- 网格格式：regular grid、quantized mesh、heightmap，还是自定义 mesh。
- 法线来源：数据自带、CPU 计算、GPU 计算，还是 shader 估算。
- tile 边缘是否使用 skirt 或边缘对齐策略来避免裂缝。
- 不同 LOD 邻接时如何处理 T-junction 和接缝。

当前项目的地形数据源选择应单独记录为实现约束，而不是写死为架构常量：

- 当前 RGB 高度图沿用 Mapbox 标准 RGB terrain PNG，单瓦片尺寸为 `514×514`。
- 该格式体积较大，且属于有损编码路径，适合作为现阶段的可用数据源，不代表最终长期方案。
- `cesium-native` 的 `QuantizedMesh` 是二进制、无损、并且为三角剖分优化的格式；在相同三角形数下通常能表达更精细的地形形状。
- 后续如果数据源、网格格式或分发方式发生变化，应优先更新数据源说明和 Provider contract，而不要把当前 PNG 选择误写成固定不变的格式承诺。

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
