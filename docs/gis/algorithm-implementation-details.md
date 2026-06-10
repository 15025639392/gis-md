# 算法实现细节手册

本文件把地球引擎关键算法拆成可实现、可测试、可调试的契约。AI 实现算法时，不能只写“实现 culling/LOD/picking”，必须说明输入、输出、坐标空间、误差模型、边界情况和测试样例。

## 算法实现模板

每个核心算法都应写清：

- 输入类型和单位。
- 输出类型和单位。
- 坐标空间：cartographic、ECEF、ENU、view、clip、screen。
- 依赖的项目约定和官方来源。
- 误差容差。
- 失败或无结果表示。
- 边界情况。
- 单元测试样例。
- debug 可视化或日志字段。

## 经纬高到 ECEF

输入：

- longitude、latitude：radian 或明确标注 degree。
- height：meter。
- ellipsoid：semi-major axis、flattening。

输出：

- ECEF Cartesian3，单位 meter。

关键步骤：

1. 计算 sin/cos latitude、longitude。
2. 根据椭球参数计算第一偏心率平方。
3. 计算 prime vertical radius。
4. 输出 x/y/z。

边界情况：

- 赤道。
- 本初子午线。
- 极区。
- height 为负。
- degree/radian 误用。

测试：

- lon=0, lat=0, h=0 时 x 接近 WGS84 semi-major axis。
- 北极附近 z 接近 semi-minor axis。

## ECEF 到经纬高

该算法比正向转换更容易出错。可使用成熟实现或迭代法，但必须测试。

要求：

- 支持地表点。
- 支持高空点。
- 支持地下或负高度点。
- 结果 longitude/latitude 单位明确。

禁止：

- 用球体公式冒充 WGS84 椭球反算。

## ENU 局部坐标

输入：

- 原点 cartographic。
- 点 ECEF。

输出：

- east、north、up，单位 meter。

要求：

- ENU 原点必须保存。
- 不同 ENU 原点的坐标不能直接混用。
- 适合局部模型、测量辅助和相机附近精度优化。

测试：

- 原点点转换后应接近 0,0,0。
- 原点东侧小位移 east 为正。
- 原点北侧小位移 north 为正。

## Ray 与椭球求交

输入：

- ray origin：ECEF 或 world。
- ray direction：归一化向量。
- ellipsoid。

输出：

- 无命中，或最近正向交点。

关键步骤：

1. 把 ray 变换到 unit sphere 空间。
2. 解二次方程。
3. 选择最小非负 t。
4. 变回 ECEF。

边界情况：

- ray 从椭球外指向地球。
- ray 从椭球内向外。
- ray 与椭球相切。
- ray 背向地球。
- 判别式接近 0。

测试：

- 屏幕中心 pick 应命中地球。
- 看向天空时返回 none。

## Screen 到 Pick Ray

输入：

- screen x/y。
- viewport width/height。
- devicePixelRatio。
- inverse view-projection matrix。

输出：

- world ray。

要求：

- 正确处理屏幕密度（@1x/@2x/@3x 和 Android density）。
- 正确处理渲染 surface 尺寸和逻辑 viewport 尺寸的差异。
- y 轴方向明确。

常见错误：

- 用逻辑像素当 framebuffer 像素（混淆 iOS points 与 pixels、Android dp 与 px）。
- 忘记 view/surface 在屏幕中的偏移（如安全区域 inset、状态栏高度）。
- NDC y 方向写反。

## Arcball / Virtual Trackball 拖拽旋转

用于地球 orbit/旋转交互。该算法是交互算法，不替代真实坐标转换、拾取或地球表面定位。

输入：

- startX/startY：拖动起点，单位 screen px 或 framebuffer px，必须与 viewport 单位一致。
- endX/endY：拖动终点，同上。
- viewport width/height：像素。
- verticalFov：radian。
- cameraDistance：与 globeRadius 相同世界单位。
- globeRadius：用于交互映射的参考半径。单位化 demo 可取 1；真实 ECEF 地球需使用 meter 或 camera-relative 后的一致单位。
- currentRotation：单位四元数。

输出：

- nextRotation：单位四元数。
- optional angularVelocity：用于惯性，单位 rad/s。
- optional inertiaAxis：单位向量。

关键步骤：

1. 根据当前相机距离估算屏幕上的地球投影半径：

```text
focalLengthPx = viewportHeightPx / (2 * tan(verticalFov / 2))
projectedRadiusPx = focalLengthPx * globeRadius / cameraDistance
```

2. 将屏幕点映射到以 viewport center 为原点的虚拟球：

```text
nx = (x - viewportWidth / 2) / projectedRadiusPx
ny = (viewportHeight / 2 - y) / projectedRadiusPx
len2 = nx * nx + ny * ny
if len2 <= 1:
  p = normalize([nx, ny, sqrt(1 - len2)])
else:
  p = normalize([nx, ny, 0])
```

3. 计算旋转轴和角度：

```text
axis = cross(from, to)
axisLength = length(axis)
dotValue = clamp(dot(from, to), -1, 1)
angle = atan2(axisLength, dotValue)
```

4. 当 `axisLength` 小于阈值时忽略该样本；否则：

```text
delta = angleAxis(angle, axis / axisLength)
nextRotation = normalize(delta * currentRotation)
```

边界情况：

- 拖动点在虚拟球外：投影到 z=0 的双曲/平面区域并归一化，避免屏幕边缘失控。
- 双指缩放后：必须重新使用当前 `cameraDistance` 计算 `projectedRadiusPx`，否则拖拽距离不一致。
- viewport 极窄或高度为 0：半径至少 clamp 到 1 px，并跳过非法输入。
- 极小位移：axis length 接近 0 时返回原旋转。
- 四元数漂移：每次累计后 normalize。

测试：

- 同一缩放级别下，相同像素拖动产生稳定角度。
- 放大后同样像素拖动产生更小角度，缩小后产生更大角度。
- 水平、垂直、斜向和反向拖拽都应让地球沿用户输入方向自然跟手，不能出现方向符号在局部坐标系切换时反转。
- 连续从南向北拖过屏幕中心和极区，不能出现 pitch clamp 卡顿。
- 连续围绕北极和南极做环形拖拽，旋转轴、角速度和相机姿态应连续，不能出现抖动、翻转或 heading/roll 突跳。
- 多次大幅拖动后四元数长度仍接近 1。

debug：

- 输出 projectedRadiusPx、angle、axis、angularVelocity。
- 可选显示虚拟球投影圆，辅助调试触控手感。

## Arcball 拖拽惯性

输入：

- delta rotation angle：相邻拖动样本得到的旋转角，radian。
- delta time：相邻样本时间，second。
- axis：相邻拖动样本的旋转轴，单位向量。
- frameDt：渲染帧间隔，second。
- dampingPerSecond：阻尼系数，1/second。
- maxAngularVelocity：最大角速度，rad/s。

输出：

- 每帧惯性 delta quaternion。
- 衰减后的 angularVelocity。

关键步骤：

1. 拖动中估算角速度：

```text
if 0 < sampleDt < maxSampleDt:
  instantaneous = min(angle / sampleDt, maxAngularVelocity)
  angularVelocity = mix(angularVelocity, instantaneous, smoothing)
  inertiaAxis = axis
```

2. pointer-up 后每帧推进：

```text
angle = angularVelocity * frameDt
rotation = normalize(angleAxis(angle, inertiaAxis) * rotation)
angularVelocity *= exp(-dampingPerSecond * frameDt)
```

3. 当角速度低于阈值时置 0。

边界情况：

- 新 pointer-down、pinch、fly-to、相机 reset、场景销毁时必须清零。
- sampleDt 过大时不能用作速度样本，避免应用切后台/卡顿后一帧飞转。
- inertiaAxis 必须归一化；非法轴时不启动惯性。
- 低 FPS 或掉帧时必须用真实 `frameDt`，不能按固定帧率。

测试：

- 快速 swipe 后，pointer-up 之后至少若干帧姿态继续变化。
- 惯性会随时间衰减并最终停止。
- pinch 后惯性立即停止。
- 连续触摸不会继承上一次惯性的残余速度。

## Frustum Plane 提取

输入：

- view-projection matrix。

输出：

- six planes，单位和归一化状态明确。

要求：

- plane normal 方向统一。
- 支持点/球/AABB/OBB 测试。
- 保守判断，避免错误剔除可见对象。

测试：

- 相机前方对象可见。
- 相机后方对象不可见。
- 贴近 near/far plane 的对象有容差。

## Horizon Culling

地球背面瓦片应被剔除，但 horizon culling 必须保守。

输入：

- camera ECEF。
- tile bounding volume。
- ellipsoid。

输出：

- visible / occluded / uncertain。

要求：

- 对 uncertain 返回 visible。
- 高空视角和近地视角都要测试。
- 不得错误剔除地平线附近瓦片。

debug：

- 显示被 horizon culling 剔除的 tile 数。
- 可视化 tile bounding volume。

## TileKey 到 Bounds

输入：

- TileScheme。
- z/x/y。

输出：

- Rectangle 或 projected bounds。

要求：

- y 轴方向明确。
- bounds 是否半开区间明确。
- 支持世界边界。
- 支持反经线或 provider bounds。

测试：

- z=0 覆盖世界范围。
- XYZ y=0 是北侧。
- TMS y=0 是南侧，或按 scheme 定义。

## Bounds 到 TileRange

输入：

- rectangle。
- zoom。
- TileScheme。

输出：

- x/y range。

要求：

- clamp 到可用范围。
- 处理跨反经线 bbox。
- 避免浮点边界导致多取或漏取。

测试：

- 小 bbox 命中至少一个 tile。
- 世界 bbox 命中完整范围。
- 跨 180 度经线拆分或归一化正确。

## Screen-Space Error

用于 terrain 和 3D Tiles LOD。

输入：

- geometricError。
- camera distance。
- viewport height。
- field of view。

输出：

- screen-space error，单位 pixel。

要求：

- distance 定义稳定，避免接近 0。
- FOV 单位明确。
- threshold 可配置。
- debug 输出每个 tile 的 SSE。

禁止：

- 只按 zoom 或距离硬编码 LOD，不记录误差模型。

## Tile Replacement

目标：

- 避免白屏。
- 避免父子瓦片闪烁。
- 避免过期请求污染当前帧。

关键规则：

- desired tile 不 ready 时使用 ancestor fallback。
- 子瓦片 ready 后再替换父瓦片。
- 可选择 all-children-ready 策略。
- render queue 必须基于当前 frame state。

测试：

- 子瓦片慢加载时不留白。
- 快速移动相机时旧 tile 不覆盖新 tile。

## Terrain Mesh 生成

输入：

- heightmap 或 quantized mesh。
- tile bounds。
- ellipsoid。

输出：

- vertex buffer、index buffer、bounding volume。

要求：

- 高度基准明确。
- skirt 或接缝策略明确。
- 法线来源明确。
- 不同 LOD 邻接有处理方案。

测试：

- tile 边界没有明显裂缝。
- 高程单位错误能被测试发现。

## Polyline 渲染

线宽通常不能依赖原生 GL line width。

策略：

- CPU 或 GPU 生成 billboard/strip。
- 处理 join/cap。
- 处理屏幕像素宽度和米宽度差异。
- 长线跨反经线时拆分。

测试：

- 不同 zoom 下线宽符合设计。
- 折线 join 不破碎。
- picking 能返回 feature id。

## Polygon 三角化

输入：

- outer ring。
- holes。
- CRS 和高度模式。

输出：

- triangles。

要求：

- ring 闭合。
- 自相交检测。
- holes 处理。
- 大 polygon 可切分。
- 贴地 polygon 与地形 z-fighting 有策略。

复杂 polygon 应优先使用成熟三角化库，不要手写半吊子 ear clipping。

## Label Placement

标注避让必须独立于业务图层。

输入：

- label candidates。
- priority。
- screen bounds。
- collision boxes。

输出：

- accepted labels。

要求：

- 按优先级排序。
- 支持 zoom/distance 可见性。
- 支持屏幕边界裁剪。
- 不应每帧全量重排所有静态 label。

测试：

- 高密度点位不大面积重叠。
- 选中对象 label 可提升优先级。

## Cache Eviction

输入：

- cache entries。
- budget。
- current visible set。
- layer priority。

输出：

- evicted entries。

策略：

- 可见 tile 不淘汰。
- parent fallback 有保留价值。
- 最近使用和加载成本共同决定。
- GPU cache 和 decoded cache 可以不同策略。

测试：

- 超预算后缓存下降。
- 当前可见 tile 不被误删。
- 图层销毁后资源释放。

## Request Scheduling

输入：

- request candidates。
- current visible set。
- priority。
- concurrency budget。

输出：

- started、queued、cancelled。

要求：

- 当前可见优先。
- 屏幕中心优先。
- 旧 frame 请求可取消或标记过期。
- 失败重试有上限。

测试：

- 快速移动相机时过期请求不覆盖当前状态。
- 弱网下当前视域优先加载。

## 算法交付要求

每次新增或修改核心算法，最终说明必须包含：

- 算法名称。
- 输入输出和单位。
- 坐标空间。
- 误差容差。
- 边界情况。
- 测试结果。
- debug 指标。
