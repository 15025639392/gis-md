# 图形学与 GPU 渲染管线

地球引擎的图形部分不是“把数据画出来”这么简单。它需要处理地球尺度精度、GPU 管线、shader、材质、深度、透明、后处理、资源生命周期和性能预算。AI 修改图形代码时，必须先说明渲染管线、坐标空间、资源归属和验证方式。

## 渲染后端

本项目通过 `RenderDevice` 抽象支持多后端。必须明确目标后端：

- Metal 2（iOS 主后端）
- OpenGL ES 3.0（Android 默认后端）
- Vulkan 1.1（Android 可选后端，未来主推）
- 桌面调试后端：OpenGL 4.1（macOS）、OpenGL 3.3 / Vulkan（Windows/Linux）

不同后端的能力差异会影响：

- instancing
- floating point texture
- multiple render targets
- depth texture
- uniform buffer / storage buffer
- compute shader
- shader language（MSL vs GLSL ES vs SPIR-V）
- context/device lost 处理
- 资源驱逐策略（Metal resource eviction vs GL context lost vs Vulkan device lost）

AI 不得假设 Metal、OpenGL ES 和 Vulkan 能力相同。

### 移动端 GPU 约束

移动端 GPU 通常使用 tile-based deferred rendering（TBDR）架构（Apple GPU、ARM Mali、Qualcomm Adreno 部分）。与桌面 GPU 的 immediate mode 有重要差异：

- **避免在片段着色器中动态计算纹理坐标**：TBDR 的 binning pass 依赖固定的几何信息。
- **避免大量 discard/clip 操作**：会破坏 TBDR 的 hidden-surface removal 优化。
- **避免频繁切换 render target**：tile memory 通常有限（~32-256 KB per tile），多次 store/load 代价高。
- **优先使用 MSAA resolve 而非全屏后处理抗锯齿**：TBDR 的 MSAA 几乎零成本（tile memory 内的 resolve）。
- **纹理格式**：移动端对 ASTC（iOS/Android）和 PVRTC（iOS legacy）有原生硬件支持；ETC2 在 Android GL ES 3.0 上强制支持。
- **最大纹理尺寸**：高端移动端通常 8192-16384 px，中低端 4096 px。引擎必须在启动时检测 `RenderDevice::maxTextureSize()`。
- **maximum varying/register 数量**：移动端通常比桌面更少，shader 复杂度必须控制。

## 坐标空间管线

必须明确每个顶点经历的坐标空间：

```text
geographic/cartographic
-> ECEF or projected/world
-> local origin / camera-relative
-> view space
-> clip space
-> NDC
-> screen space
```

常见矩阵：

- model matrix
- view matrix
- projection matrix
- modelView matrix
- modelViewProjection matrix
- normal matrix

地球尺度场景中，CPU 用 double（C++ 原生 `double` 或 GLM::dvec3），GPU 必须使用 float32（Metal/GL ES/Vulkan 均支持）。移动端 GPU 对 double 无硬件支持或性能极差。大坐标必须通过 camera-relative、origin rebasing、high/low split 或局部 ENU 解决精度问题。

移动端 TBDR GPU 的 tile buffer 精度通常为 16-bit（RGB10_A2 或 RGBA8），深度精度有限。在高空视角（地球整体可见）时，务必使用 logarithmic depth 或 reversed-Z 避免 z-fighting。

## Shader 设计

Shader 必须明确：

- 输入 attributes。
- uniforms / uniform buffers。
- varyings。
- texture samplers。
- precision qualifier。
- 坐标空间。
- gamma / color space。
- 是否参与 picking pass、depth pass、shadow pass。

不要在 shader 中写业务规则、provider 坐标偏移、权限判断或临时纠偏。shader 只应表达已校正后的渲染数据。

## 材质系统

材质至少区分：

- unlit material：图标、业务颜色、UI-like overlay。
- lit material：地形、模型、3D Tiles。
- PBR material：glTF、真实感模型。
- procedural material：水、云、热力、风场。
- classification / thematic material：分级设色、风险图、科学数据。

业务分析颜色通常应保持语义稳定，不应被光照、曝光或 tone mapping 改到无法识别。

## 纹理

纹理设计必须明确：

- source：平台图片解码器输出（iOS: CGImage、Android: BitmapFactory）、原始像素缓冲区（stb_image 回退）、瓦片数据、data texture、atlas。
- format：RGBA8、RGB8、R16F、RGBA16F、depth 等。
- color space：sRGB 或 linear。
- mipmap。
- wrap mode。
- filter mode。
- anisotropy。
- memory cost。

影像瓦片、图标 atlas、科学栅格、深度纹理和 picking texture 不能混为一种纹理类型。

底图瓦片纹理的上传节流、父子替换、图层混合和失败降级见 `basemap-tile-rendering.md`。

## Buffer 与几何

几何资源必须明确：

- vertex buffer。
- index buffer。
- instance buffer。
- attribute layout。
- interleaved 或 separate。
- dynamic / static usage。
- update range。
- bounding volume。

大量点、标注、模型实例应优先考虑 instancing 或 batching。不要为每个 feature 创建独立 draw call。

## 深度与 Z-Fighting

地球场景常见问题：

- near plane 太小。
- far plane 太大。
- 地表 polygon 与地形共面。
- 透明对象写 depth。
- 多层影像或面状覆盖物没有 polygon offset。

策略：

- 合理设置 near/far。
- 使用 logarithmic depth 或 reversed-Z，如果后端支持且项目需要。
- 对贴地 polygon 使用 terrain draping、depth offset 或 classification pass。
- 明确 depth test 和 depth write。
- 透明对象通常需要特殊排序或单独 pass。

## 透明与混合

透明渲染必须明确：

- blend mode。
- premultiplied alpha。
- depth test。
- depth write。
- sort key。
- order-independent transparency 是否需要。

透明面、云、雾、水、大气和标注都可能互相影响。不能只靠 render order 魔法数字长期维护。

## 多 Pass 渲染

常见 pass：

- depth prepass
- color pass
- picking pass
- shadow pass
- classification pass
- terrain depth pass
- postprocess pass
- atmosphere pass
- label/billboard pass

每个 pass 必须说明输入、输出、framebuffer、清理规则和资源复用。

## 后处理

后处理包括：

- FXAA / MSAA resolve。
- bloom。
- tone mapping。
- color grading。
- fog。
- atmosphere composition。
- depth visualization。
- outline highlight。

后处理不能改变业务数据值。对科学栅格、风险分级、告警颜色，应避免被全局色调映射破坏语义。

## Picking 渲染

GPU picking 需要独立规则：

- picking id 分配。
- id 到 feature 的映射。
- framebuffer 尺寸和 DPR。
- MSAA 是否影响 id 读取。
- 透明对象是否参与。
- label、billboard、3D Tiles feature 的优先级。

Picking pass 不应使用和 color pass 完全相同的材质，否则容易受透明、光照、后处理影响。

## 资源生命周期

所有 GPU 资源必须可追踪：

- 创建位置。
- 所属 layer / tile / primitive。
- 引用计数或所有权。
- 更新策略。
- 释放时机。
- context lost / device lost 恢复（Metal: MTLDevice 资源驱逐通知；GL ES: EGL context lost；Vulkan: VK_ERROR_DEVICE_LOST）。
- 应用后台/前台切换时的资源策略（GPU 资源是否保留、是否需要重建）。

移动端 GPU 可能随时驱逐未使用的资源（Metal resource eviction）。引擎必须在应用进入前台时检查资源有效性并重建。

瓦片卸载、图层隐藏、场景销毁、样式切换和数据更新都可能触发资源释放。AI 不得只实现创建不实现释放。

## 渲染调度

必须明确：

- requestRender 模式还是 continuous render loop。
- 哪些状态变化触发重绘。
- 动态对象、天气、时间轴是否需要持续渲染。
- 空闲时是否停止渲染以省电（移动端电量预算，见 `technology-decisions.md`）。
- 线程池解析和主线程上传 GPU 的边界。
- 应用挂起到后台时是否停止渲染循环（iOS CADisplayLink 自动暂停；Android Choreographer 需手动停止）。

静态地球视图不应无意义地满帧渲染，除非有动画或交互。移动端 60 FPS 连续渲染会显著耗电；无交互时应降至 30 FPS 或暂停。

## 性能指标

至少观测：

- FPS / frame time。
- CPU update time。
- GPU time，如果可用。
- draw calls。
- triangles / points。
- texture count 和估算显存。
- buffer count 和估算显存。
- shader/program/pipeline 数量。
- framebuffer 数量。
- 每帧资源创建次数。

性能优化要先有指标，再改结构。不要盲目合并或拆分 draw call。

## Debug 视图

建议提供：

- wireframe。
- normal visualization。
- depth visualization。
- overdraw visualization。
- tile boundary。
- bounding volume。
- frustum。
- shadow map preview。
- texture atlas preview。
- picking id preview。
- GPU resource table。

这些 debug view 应可开关，不能污染正式 UI。

## 常见错误

- 每帧创建 shader、material、texture 或 buffer。
- 把地理坐标直接作为 GPU 顶点。
- 用 shader 魔法偏移修正瓦片错位。
- 忽略屏幕密度（Retina/@2x/@3x / Android density）导致 picking 偏移。
- 透明对象写 depth 导致后续对象消失。
- 后处理改变业务分级颜色。
- context lost 后无法恢复。
- 图层隐藏但 GPU 资源不释放。
- 移动端使用桌面级别纹理和后处理。
- 每帧创建新的 MTLCommandBuffer 或 GL command encoder 而不复用。
- Metal resource eviction 后未重建纹理和 buffer 导致黑屏。
- 在 Metal 和 GL ES 后端之间使用不同的 shader 精度导致渲染不一致。

## 验收清单

图形相关改动至少验证：

- iOS 和 Android 首屏非空白。
- RenderDevice 能力检查和降级路径（Metal → 无降级选项；GL ES → 回退到较低版本的 ES 或错误提示）。
- 近地不抖动，远地不闪烁。
- depth、透明、标注和后处理没有明显互相破坏。
- picking 在不同屏幕密度下准确（iOS @1x/@2x/@3x、Android mdpi/xhdpi/xxhdpi）。
- 图层切换和瓦片卸载后 GPU 资源下降。
- context lost / device lost 有恢复或明确错误提示。
- 应用后台/前台切换后渲染正常。
- 移动端性能和内存可接受（低端和中端设备分别测试）。
