# 性能、数据约束与底层稳定性

地球引擎性能的核心不只是 draw call、shader 或 GPU 调参。真正决定性能上限的，通常是数据是否被约束、预处理、索引、切片、分级、缓存，以及底层数学、调度、资源生命周期是否正确稳定。

AI 处理性能问题时，必须先判断瓶颈来自数据管线还是渲染管线。不要在渲染层用 hack 掩盖数据过载、坐标错误、资源泄漏或调度不稳定。

## 性能优先级

优先级从高到低：

1. 数据约束：限制输入规模、格式、字段、精度和有效范围。
2. 数据预处理：切片、简化、索引、金字塔、压缩、分块。
3. LOD 策略：按屏幕误差和业务意义加载必要数据。
4. 调度与缓存：请求、解析、上传、释放都可控。
5. 底层正确性：坐标、矩阵、拾取、tile bounds、资源生命周期稳定。
6. 渲染优化：batch、instancing、shader、draw call、后处理。

如果前四项没有做好，单纯优化 shader 或 draw call 通常收益有限。

## 数据约束

每种数据源必须定义约束：

- 最大 feature 数。
- 最大 geometry 顶点数。
- 最大 tile 大小。
- 最大纹理尺寸。
- 最大属性字段数量。
- 最大单 feature 属性体积。
- 最大时序窗口。
- 最大点云点数。
- 最大 3D Tiles 同屏 tile/content 数。
- 最大并发请求数。

超出约束时必须有策略：

- 拒绝加载。
- 服务端预处理。
- 自动切片。
- 简化或抽稀。
- 降级显示。
- 分页或按视域加载。
- 提示用户数据过大。

不要让任意 GeoJSON、任意图片、任意模型直接进入渲染管线。

## 数据预处理

推荐预处理：

- 矢量：tile 化、simplify、topology preserve、空间索引、属性裁剪。
- 栅格：COG、金字塔、nodata、重投影、色带预计算。
- 地形：quantized mesh、heightmap 金字塔、skirt、法线、压缩。
- 点云：LOD、octree、classification、颜色压缩、空间索引。
- 3D Tiles：geometricError、bounding volume、implicit tiling、纹理压缩。
- 轨迹：分段、抽稀、时间索引、速度/方向预计算。

预处理产物必须记录：

- 输入版本。
- 处理参数。
- CRS。
- 单位。
- 精度损失。
- 生成时间。

## 索引策略

地球引擎常用索引：

- quadtree：地图瓦片、影像、矢量 tile。
- octree：点云、三维空间。
- R-tree：二维 bbox 查询。
- BVH：模型和复杂 mesh picking。
- time index：时序数据。
- feature id index：picking 和属性查询。

索引必须和数据生命周期绑定。数据更新后索引必须失效或增量更新，不能静默使用过期索引。

## LOD 与精度预算

LOD 不只是性能优化，也是正确性策略。必须定义：

- 屏幕误差阈值。
- 几何误差阈值。
- 样式可见层级。
- 标注可见层级。
- 属性精度是否随 LOD 变化。
- 分析数据是否允许使用简化版本。

严肃分析不能默认使用渲染 LOD 数据。分析数据和显示数据应有边界。

## Worker 与主线程

应放入 worker 的任务：

- 大瓦片解码。
- MVT/GeoJSON 解析。
- 几何简化。
- 三角化。
- 点云解码。
- 3D Tiles content 解析。
- 统计和聚合。

主线程应负责：

- 最终状态合并。
- GPU 上传。
- 用户交互。
- 小规模同步计算。

Worker 输出必须可取消或可丢弃。过期 worker 结果不能覆盖当前场景状态。

## 缓存与内存

缓存必须有预算：

- raw data cache 上限。
- decoded data cache 上限。
- GPU texture cache 上限。
- mesh/buffer cache 上限。
- per provider 上限。
- 全局上限。

缓存淘汰应考虑：

- 当前可见。
- 最近使用。
- 加载成本。
- 图层优先级。
- 父瓦片 fallback 价值。
- 内存压力。

缓存命中率低时，先检查 tile plan 和请求策略，不要盲目扩大缓存。

## 底层正确性门禁

以下问题未稳定前，不应谈高级性能优化：

- 坐标转换测试不稳定。
- tileToBounds / boundsToTile 有边界错误。
- picking 在 DPR 下不准。
- 请求取消后旧数据能覆盖新状态。
- 图层隐藏后资源不释放。
- 每帧创建 GPU 资源。
- LOD 在临界值抖动。
- cache key 缺少 provider/layer/style/time。
- 反经线或极区导致无限 tile 请求。

这些问题会制造“性能问题”的假象。

## 稳定性压力场景

每次性能优化后至少跑：

- 快速连续缩放。
- 快速旋转地球。
- 高低空频繁切换。
- 切换多个底图。
- 打开/关闭大矢量图层。
- 加载失败/弱网/超时。
- 长时间运行 30 分钟以上。
- 移动端或低性能设备。
- 反经线和高纬度区域。

## 性能指标解释

指标要能定位层级：

- 网络慢：request latency、失败率、队列长度。
- 解析慢：worker time、main thread parse time。
- 上传慢：texture upload count/time。
- 渲染慢：draw calls、triangles、GPU time。
- 内存高：raw/decoded/GPU cache。
- 抖动：LOD switching、request churn、tile replacement。

只报告 FPS 不够。FPS 低只是结果，不是原因。

## 优化禁区

- 为了快而删除 CRS 校验。
- 为了快而跳过 geometry validity。
- 为了快而让 shader 修正坐标偏移。
- 为了快而使用不可追踪全局缓存。
- 为了快而取消资源释放。
- 为了快而把分析数据替换成渲染简化数据。
- 为了快而隐藏错误瓦片或权限问题。

性能优化不能牺牲空间正确性和工程可诊断性。

## AI 性能排查流程

1. 明确场景：数据类型、规模、设备、视角、操作。
2. 收集指标：网络、解析、调度、上传、渲染、内存。
3. 检查数据约束：是否超出项目定义上限。
4. 检查预处理：是否需要 tile、LOD、索引、简化。
5. 检查底层正确性门禁。
6. 再考虑渲染优化。
7. 给出可复现 benchmark 或回归测试。

没有指标和复现场景时，不应给出确定性性能结论。
