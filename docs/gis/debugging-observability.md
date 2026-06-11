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

- 地球空白：检查渲染 surface、相机、near/far、shader 编译、首个 tile、context/resource lost。
- 图形异常：检查 shader 编译、buffer attribute layout、texture format、framebuffer、depth state、blend state 和 postprocess。
- 瓦片错位：检查 CRS、tile scheme、y 轴方向、bounds、经纬度顺序。
- 地形裂缝：检查 skirt、邻接 LOD、边缘高程、index buffer。
- 近地抖动：检查 float32 精度、相机相对坐标、origin rebasing。
- 瓦片闪烁：检查请求取消、旧请求覆盖、LOD hysteresis、cache eviction。
- 点击不准：检查 screen to ray、屏幕密度（@2x/@3x）、渲染 surface 尺寸、地形/椭球拾取优先级。
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

## Bug 复盘：Atmosphere 开启后地表蓝色穿孔

### 现象

Android MinimalGlobe 开启 atmosphere pass 后，近地底图出现大面积蓝色颗粒/穿孔，看起来像大气或背景覆盖到地表。关闭 atmosphere 后，底图恢复正常。

### 影响范围

- 影响后端：OpenGL ES 3.0。
- 影响路径：任何在一帧末尾留下 `depthWrite=false`、`blend=true` 或 polygon offset 状态的 background/overlay pass，都可能污染下一帧。
- 可见症状：地表 depth test 对 stale depth 运行，SurfaceTile 产生随机空洞，后面的 sky/atmosphere/background 从洞里透出。

### 关键观察

- `atmosphere off`：底图正常，`draw=26`，`glError=0`，约 60 FPS。
- `atmosphere on`：底图蓝色颗粒，`draw=27`，`glError=0`，约 60 FPS。
- 将 atmosphere fragment 临时改成固定半透明红色后，高空视角红色只出现在地球外背景，说明 pass order/depth occlusion 本身可以工作。
- 恢复真实 atmosphere 后仍出现蓝色穿孔，说明不是散射公式直接把地表染蓝。
- 在 `RenderDeviceGLES::beginFrame()` 清 depth 前恢复 GL 状态后，真实 atmosphere 开启时近地底图恢复正常。

### 排除法过程

| 实验 | 目的 | 结果 | 结论 |
| --- | --- | --- | --- |
| 临时屏蔽 atmosphere command | 判断基座是否坏 | 底图正常 | SurfaceTile/imagery 基座不是主因 |
| atmosphere fragment 固定输出红色 | 判断散射公式是否主因 | 红色实验没有复现蓝色散射公式路径 | 散射公式不是第一个错误状态 |
| 多视野截图：近地/中空/高空 | 判断是否 depth/order 全局错误 | 高空红色 pass 被地球遮挡 | 基本排除 pass order/depth test 全局错误 |
| 检查跨帧 GL 状态 | 判断是否 state leakage | `depthWrite=false` 可跨帧残留到下一帧 clear 前 | 指向 GLES 状态恢复缺失 |
| beginFrame 强制恢复状态 | 证伪/验证修复 | atmosphere 开启后近地底图正常 | 根因成立 |

### 根因

`RenderDeviceGLES::submit()` 按命令切换 GL 状态，但帧结束后不恢复全局状态。Atmosphere/background/overlay 命令会设置：

```cpp
cmd.depthWrite = false;
cmd.blend = true;
```

GLES 后端据此调用：

```cpp
glDepthMask(GL_FALSE);
glEnable(GL_BLEND);
```

下一帧 `beginFrame()` 在没有先恢复 `glDepthMask(GL_TRUE)` 的情况下执行：

```cpp
glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
```

在 depth mask 仍为 false 时，depth buffer 不能可靠回到本帧 clear 状态。后续 SurfaceTile 用 stale depth 做 reverse-Z `GL_GEQUAL` 测试，部分地表像素被错误丢弃，背景/atmosphere 从这些空洞透出。

### 修复

每帧开始、clear 之前恢复 frame-global GL 状态：

```cpp
glDepthMask(GL_TRUE);
glDisable(GL_BLEND);
glDisable(GL_POLYGON_OFFSET_FILL);
glClearDepthf(0.0f);
glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
```

同时，fullscreen atmosphere vertex shader 必须显式绑定 attribute location，避免 GLES 链接器分配和后端固定 attribute 0 输入不一致：

```glsl
layout(location = 0) in vec2 a_position;
```

### 渲染状态契约

- `beginFrame()` 必须建立后端的 canonical frame state，不能依赖上一帧最后一个 command 的状态。
- `glClear` 前必须确认 depth write mask 为 true。
- background/overlay/transparent pass 可以关闭 depth write，但不能把该状态泄漏到下一帧。
- 后端命令缓存只优化同一 submit 内的状态切换；跨 frame 必须重新声明关键状态。
- 所有新 pass 都必须声明：depth test、depth write、blend、cull、polygon offset、attribute layout。

### 验证方式

- Android 真机 `bash scaffold/build_and_install.sh` 构建安装启动。
- `adb logcat` 观察 `GLES submit ... glError=0`。
- 对比截图：
  - `tmp/android-qa/atmosphere-disabled-baseline.png`：atmosphere 关闭基线。
  - `tmp/android-qa/multiview-atmos-diagnostic-*.png`：固定红色 pass 多视野诊断。
  - `tmp/android-qa/rootcause-depthmask-reset-near.png`：修复后真实 atmosphere 开启的近地结果。
- 预期：真实 atmosphere 开启时，近地底图无蓝色穿孔；高空背景/limb 可见大气，地球本体不被 fullscreen pass 覆盖。

### 以后遇到类似问题先查

1. 关闭可疑 overlay/pass 做 A/B。
2. 将可疑 pass fragment 改成固定诊断色，确认它实际画到哪里。
3. 抓多视野截图，不只看近地。
4. 检查 `beginFrame()` 是否恢复 `glDepthMask`、blend、polygon offset、cull、depth func。
5. 检查 shader attribute 是否显式 `layout(location=...)` 且与后端 vertex attrib pointer 一致。
6. 不要优先调 atmosphere/fog 颜色参数；先证明 depth 和 GL state 契约成立。
