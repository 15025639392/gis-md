# 3D Tiles 开发规则

3D Tiles 用于大规模 3D 地理空间内容，不是普通模型加载器。AI 实现或修改 3D Tiles 时，必须遵守官方 specification，并把 tileset traversal、bounding volume、geometric error、refinement 和 content 生命周期分开处理。

## 核心对象

- `tileset.json`：入口文件，包含 asset、geometricError、root、extensions。
- `tile`：层级节点，包含 boundingVolume、geometricError、refine、transform、content、children。
- `content`：实际内容，例如 b3dm、i3dm、pnts、cmpt、glTF、subtree。
- `boundingVolume`：box、region、sphere 或扩展形式。
- `geometricError`：LOD 误差指标。
- `refine`：ADD 或 REPLACE。

## Traversal

Traversal 必须明确：

- 输入：camera、frame state、tileset root。
- 输出：本帧要渲染的 tile 集合、要请求的 tile 集合、要卸载的 tile 集合。
- 判断：frustum culling、screen-space error、viewer request volume、refine 策略。
- 稳定性：避免相机轻微移动导致 tile 抖动切换。

不要把 traversal、网络请求、content 解析和 draw call 提交写在一个函数里。

## Bounding Volume

Bounding volume 处理必须考虑：

- tile transform 是否应用。
- region 单位通常是 radian，不要误用 degree。
- box/sphere/region 的世界坐标解释。
- parent/child transform 继承。
- culling 失败时优先保守显示，不要错误剔除可见内容。

## Screen-Space Error

SSE 计算必须记录：

- geometric error 来源。
- camera distance 的定义。
- viewport height。
- field of view。
- maximum screen-space error 阈值。
- dynamic SSE 或 fog/height-based 策略是否启用。

SSE 相关改动必须有可调试输出，例如每个 tile 的 error、distance、selected/refined 状态。

## Content 生命周期

每个 content 应有状态机：

- `unloaded`
- `queued`
- `loading`
- `processing`
- `ready`
- `failed`
- `expired`
- `unloading`

旧 content 不应在新状态中误渲染。失败 content 应有重试或降级策略。

## Metadata 和 Styling

3D Tiles 可能包含 batch table、feature metadata、structural metadata、style expression。实现时必须说明：

- feature id 如何获取。
- picking 如何返回 feature。
- metadata 是否参与样式。
- 样式变化是否需要重建 GPU 资源。

## 常见错误

- 把 region 的 radian 当成 degree。
- 忽略 tile transform。
- 错误实现 ADD/REPLACE refinement。
- 只按距离加载，不用 geometric error。
- 不释放已卸载 tile 的 GPU 资源。
- picking 只返回 mesh，不返回 feature id。
- 请求队列没有优先级，导致屏幕外 tile 抢占带宽。
