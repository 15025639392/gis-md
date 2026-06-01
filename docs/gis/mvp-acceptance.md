# 最小可运行地球引擎验收

本文件定义从 0 开发地球引擎的 MVP。MVP 的目标不是功能多，而是证明核心链路闭合：坐标、相机、地球、底图瓦片、调度、渲染、拾取、调试和测试。

## MVP 必须包含

- iOS 和 Android 可启动的最小示例应用。
- 一个可旋转缩放的 WGS84 地球（通过 RenderDevice 渲染，Metal on iOS、GL ES on Android）。
- 一个标准 XYZ Web Mercator 底图 provider（通过 PlatformBridge HTTP 加载）。
- 底图瓦片贴到地球表面。
- Tile debug overlay。
- 基础 picking：触控点击地球返回经纬度。
- 基础相机控制：触控 drag rotate、pinch zoom。
- 基础 diagnostics：FPS、draw calls、visible tiles、request queue。
- GoogleTest 单元测试和至少一张双平台截图验收。

## MVP 不包含

MVP 阶段可以不做：

- 3D Tiles。
- 复杂地形。
- 多 provider 国内偏移瓦片。
- 复杂点线面编辑。
- 天气、云、大气散射。
- PBR 模型。
- 离线包。

这些能力必须在核心链路稳定后逐步添加。

## 功能验收

必须验证：

- 应用启动后渲染 surface 非空白。
- 地球在视口中完整显示。
- 触控拖动可旋转。
- 捏合手势可缩放。
- XYZ 瓦片能加载并贴到地球上。
- debug overlay 能显示 z/x/y。
- 快速拖动不会被旧瓦片大面积污染。
- 触控点击地球返回合理经纬度。

## 数学验收

必须有单元测试：

- WGS84 cartographic -> ECEF。
- ECEF -> cartographic。
- degree/radian 转换。
- screen -> pick ray。
- ray -> ellipsoid intersection。
- XYZ tile z/x/y -> rectangle。
- rectangle -> tile range。

## 渲染验收

必须验证：

- 首屏非空白。
- 近地不明显抖动。
- 远地不闪烁。
- texture 数量随瓦片加载变化。
- 场景销毁后 GPU 资源释放。

## 瓦片验收

必须验证：

- visible tile set 随相机变化。
- missing tile 会进入 request queue。
- loading tile 不直接造成白屏。
- parent fallback 可用。
- 失败瓦片不会无限重试。
- 请求可以取消或过期。

## 调试验收

必须能查看：

- camera position。
- visible tile list。
- request queue。
- tile cache state。
- FPS / frame time。
- draw call。
- texture count。

## 自动化验收建议

- 单元测试覆盖 core math 和 tile scheme。
- 使用 Xcode UITest（iOS）或 Android Compose Test / ActivityScenario（Android）启动示例应用。
- 截图检查渲染 surface 非空白。
- 可选：像素检查确认地球区域不是纯色背景。
- 可选：模拟 pinch/drag 后检查 camera state 改变。

## MVP 完成定义

只有同时满足以下条件，才算 MVP 完成：

- 示例页面可运行。
- 核心数学测试通过。
- 底图瓦片显示。
- picking 返回经纬度。
- debug overlay 可用。
- 没有明显资源泄漏。
- README 能说明如何运行和验证。
