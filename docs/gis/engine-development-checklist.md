# 地球引擎开发检查清单

AI 在实现地球引擎功能时，除 `verification-checklist.md` 外，还必须检查本文件。

## 架构

- 是否把 geodesy、tile scheme、provider、renderer、camera、scheduler 分层？
- 坐标转换是否集中在核心层，而不是散落在 UI 或 shader 拼接代码中？
- 异步数据加载是否可取消？
- GPU 资源是否有释放路径？

## 坐标

- 是否明确 WGS84 ellipsoid 或其他参考体？
- 经纬度单位是 degree 还是 radian？
- 高度单位和高程基准是否明确？
- ECEF、ENU、Web Mercator、screen、clip space 是否没有混用？
- 是否考虑 float32 精度问题？

## 瓦片与数据

- 数据类型是否已在 `engine-data-catalog.md` 中归类？
- 是否填写了数据格式、来源、CRS、坐标顺序、高度基准、单位、时间维度？
- tile scheme 是否明确？
- tile bounds 是否可测试？
- y 轴方向是否明确？
- 影像、地形、矢量、3D Tiles 的 provider 边界是否清楚？
- 缓存、重试、取消、降级是否设计过？

## 渲染

- near/far plane 是否合理？
- 是否有 frustum/horizon culling？
- LOD 是否有明确误差指标？
- 是否避免每帧创建 GPU 资源？
- 是否能处理 context lost / device lost？

## 体验

- 首屏是否有可见内容或明确 loading 状态？
- 相机控制是否稳定，不穿地、不抖动？
- 快速拖动/缩放时是否不会闪烁或显示过期数据？
- debug overlay 是否能显示 tile、LOD、请求、帧率等关键信息？

## 交付

- 是否说明本次改动影响的 Provider、Renderer、Camera、Scheduler 或 Geodesy 模块？
- 是否给出可复现测试或截图验证？
- 是否列出未覆盖的极区、反经线、网络失败、设备性能风险？
- 是否更新相关知识库或项目约定？
