# 调试与观测能力

地球引擎必须内建调试能力。没有观测能力时，AI 和人类都很难判断问题来自坐标、瓦片、LOD、网络、GPU 还是相机。

## Debug Overlay

建议提供可开关 overlay：

- tile 边界。
- tile z/x/y 或 tile id。
- tile 状态：queued、loading、ready、failed、evicted。
- parent fallback、cache hit/miss、texture-ready、rendered 状态。
- LOD error / screen-space error。
- bounding volume。
- frustum。
- 相机位置、高度、heading、pitch、roll。
- FPS、frame time、draw call。
- 请求队列和缓存命中率。

Overlay 必须可关闭，不能污染正常产品 UI。

## 日志分类

日志应按领域分类：

- `geodesy`
- `tile`
- `provider`
- `scheduler`
- `terrain`
- `imagery`
- `3d-tiles`
- `renderer`
- `graphics`
- `camera`
- `picking`
- `interaction`
- `performance`

日志要能按级别关闭。高频帧日志必须采样或节流。

## 诊断快照

建议提供一键诊断快照，包含：

- 当前相机状态。
- 当前可见 tile 列表。
- 请求队列。
- cache 状态。
- 图层列表。
- renderer 资源统计。
- 最近错误。
- 当前 CRS / tile scheme / terrain provider。

快照应可复制为 JSON，方便提交 issue 或给 AI 分析。

## 常见问题定位

- 地球空白：检查 canvas、相机、near/far、shader 编译、首个 tile、context lost。
- 图形异常：检查 shader 编译、buffer attribute layout、texture format、framebuffer、depth state、blend state 和 postprocess。
- 瓦片错位：检查 CRS、tile scheme、y 轴方向、bounds、经纬度顺序。
- 地形裂缝：检查 skirt、邻接 LOD、边缘高程、index buffer。
- 近地抖动：检查 float32 精度、相机相对坐标、origin rebasing。
- 瓦片闪烁：检查请求取消、旧请求覆盖、LOD hysteresis、cache eviction。
- 点击不准：检查 screen to ray、DPR、canvas 尺寸、地形/椭球拾取优先级。
- 绘制编辑异常：检查工具状态机、临时 feature、撤销栈、吸附规则和取消清理。

## AI 调试流程

AI 遇到地球引擎 bug 时，必须先分类：

1. 坐标/CRS 问题
2. 数据/Provider 问题
3. 瓦片/LOD 问题
4. 渲染/GPU 问题
5. 相机/交互问题
6. 性能/调度问题

然后要求或生成对应证据：日志、overlay、截图、测试、诊断快照。不要直接猜测修复。
