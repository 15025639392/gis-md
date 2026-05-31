# 地球渲染引擎规则

地球渲染引擎同时受 GIS 正确性和 GPU 约束影响。AI 修改渲染相关代码时，必须说明场景坐标、GPU 资源生命周期和验证方式。

图形管线、shader、材质、纹理、buffer、深度、透明、多 pass、后处理和 GPU 性能规则见 `graphics-pipeline.md`。本文件只定义渲染引擎的高层职责。

底图瓦片渲染编排见 `basemap-tile-rendering.md`。Renderer 不应直接负责网络请求、provider 坐标纠偏或缓存策略。

## 渲染对象

常见对象包括：

- Ellipsoid / globe mesh
- Terrain tile mesh
- Imagery texture
- Vector overlay
- Polyline / polygon / label / marker
- 3D Tiles content
- glTF model
- Atmosphere / sky / lighting / shadow

每类对象都应有明确的数据来源、坐标系、生命周期和释放逻辑。

点、线、面、标注、模型和点云的样式设计必须遵守 `overlay-styling.md`，Renderer 只负责把样式高效表达为 draw call、buffer、texture 和 shader 参数。

星空、大气、光照、天气、云层、海洋和时间系统必须遵守 `environment-atmosphere-weather.md`，Renderer 需要区分真实数据、近似模型和纯视觉效果。

## 相机

相机系统必须明确：

- projection：perspective 或 orthographic。
- view matrix 的世界坐标定义。
- near/far plane 策略，避免 depth precision 问题。
- orbit、pan、zoom、tilt 的输入映射。
- 相机高度、俯仰角、最小/最大缩放限制。
- 是否禁止穿地，是否支持地下或室内场景。

地球尺度场景要特别关注 depth buffer 精度。near plane 太小、far plane 太大，会导致 z-fighting。

相机输入、手势、fly-to、follow、碰撞和状态序列化必须遵守 `interaction-system.md`。

## Culling

可见性判断应分层：

- horizon culling：地球背面的瓦片不渲染。
- frustum culling：视锥外对象不渲染。
- backface / occlusion：根据场景能力选择。
- screen-space error：控制 terrain 和 3D Tiles LOD。

Culling 逻辑必须可调试，建议提供 overlay 或日志显示瓦片状态。

## GPU 资源

所有 GPU 资源必须有生命周期：

- 创建：buffer、texture、shader、pipeline/material。
- 更新：增量更新还是重建。
- 释放：瓦片卸载、图层关闭、场景销毁。
- 恢复：WebGL context lost 或 WebGPU device lost。

不要在每帧重复创建 buffer、texture、material 或 shader。

GPU 资源所有权、context lost / device lost、资源统计和释放验收必须遵守 `graphics-pipeline.md`。

## Picking

拾取功能必须明确：

- 是 CPU ray casting、GPU color picking、depth picking，还是混合方案。
- 返回结果坐标是 screen、ECEF、cartographic，还是业务对象 id。
- 地形拾取和椭球拾取的优先级。
- 透明对象、地下对象、遮挡对象如何处理。

拾取结果、选择状态、查询面板和编辑工具必须通过 `interaction-system.md` 定义的结构化契约串联。

## 性能预算

实现前应给出初始预算，后续按项目实测修正：

- 目标帧率：例如 60 FPS 或 30 FPS。
- 单帧 draw call 上限。
- 可见瓦片数量上限。
- 纹理显存上限。
- 网络并发上限。
- 主线程解析耗时上限。

## 必须验证

地球渲染相关改动至少应验证：

- 首屏非空白。
- 旋转、缩放、平移正常。
- 近地和远地都不明显抖动。
- 地形和影像没有明显错位。
- 瓦片边界没有明显裂缝。
- 快速移动相机不会显示大量过期瓦片。
- 销毁场景或切换图层后资源能释放。
