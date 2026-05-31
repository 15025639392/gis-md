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
- 是否声明了 `TileScheme` 和 `CrsProfile`？
- 如果使用高德、百度、腾讯或私有瓦片，是否证明它不是被误当作标准 Web Mercator？
- 是否有控制点验收来证明多图层无系统性偏移？
- 如果是底图瓦片，是否遵守 `basemap-tile-rendering.md` 的可见性选择、请求调度、缓存、纹理上传和父子 fallback 策略？
- tile scheme 是否明确？
- tile bounds 是否可测试？
- y 轴方向是否明确？
- 影像、地形、矢量、3D Tiles 的 provider 边界是否清楚？
- 缓存、重试、取消、降级是否设计过？

## 渲染

- 点、线、面、标注或模型样式是否遵守 `overlay-styling.md`？
- 样式是否区分数据 schema、渲染表达和交互状态？
- 图形管线、shader、材质、纹理、buffer、深度、透明和后处理是否遵守 `graphics-pipeline.md`？
- near/far plane 是否合理？
- 是否有 frustum/horizon culling？
- LOD 是否有明确误差指标？
- 是否避免每帧创建 GPU 资源？
- 是否能处理 context lost / device lost？

## 体验

- 首屏是否有可见内容或明确 loading 状态？
- 星空、大气、光照、天气等环境效果是否可关闭或降级？
- 环境效果是否明确是 visual-only、approximate 还是 analytic？
- 相机、picking、选择、绘制、编辑、测量和时间轴是否遵守 `interaction-system.md`？
- 相机控制是否稳定，不穿地、不抖动？
- 快速拖动/缩放时是否不会闪烁或显示过期数据？
- debug overlay 是否能显示 tile、LOD、请求、帧率等关键信息？

## 交付

- 是否说明本次改动影响的 Provider、Renderer、Camera、Scheduler 或 Geodesy 模块？
- 是否给出可复现测试或截图验证？
- 是否列出未覆盖的极区、反经线、网络失败、设备性能风险？
- 是否更新相关知识库或项目约定？
