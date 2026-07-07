# 行星级深度精度 — 破碎(z-fighting)修复（2026-07-07）

> ## ⚠️ 结论更正（真机实证后）
>
> 本文档下半部分推的 **Float32 深度缓冲方案是错的方向,已实现并回退**。真机 `[DEPTHDIAG]`
> 证明离屏 FBO 的深度确实是 32-bit(`GL_RENDERBUFFER_DEPTH_SIZE=32`),但破碎**丝毫未变** ——
> 证明瓶颈**不在深度缓冲位数**。
>
> **真正的根因**:reverse-Z 配 `far/near = 1e12/150 ≈ 70亿` 的极端比,把整个球面的 z_ndc 挤进
> `~2e-5` 的**病态小值区**。精度损失发生在**深度缓冲之前** —— 顶点着色器用 **float32** 算 clip 空间
> z,值落在 2e-5 时相邻表面 1m 的深度差只有 ~1 ULP,**着色器端就分不开**了,换多大的深度缓冲都没用。
>
> **正确修法(已落地,零性能开销)**:每帧动态**收紧 near 平面**到 `max(150, 0.5×(distToCenter −
> maxRadius))`,把 z_ndc 从 2e-5 挪到 ~0.5 的良态区 → 同样的 1m 差能清晰分开。改动仅
> [SceneFrameUpdateCoordinator.cpp](../../scaffold/src/earth_engine/scene/SceneFrameUpdateCoordinator.cpp)
> 两个标量 uniform/帧。真机验证:高空静止/运动/新加载全干净,无破碎、无幽灵。
>
> **附带教训**:float 深度离屏 FBO 那版出现的"半透明幽灵地球"是**我 FBO 实现的产物**(回退后消失),
> 不是既有双绘 bug —— 之前 agent 判的 verdict B 偏了。`GLfloat`/`glGetFloatv` 等 GLES 大浮点开销
> 误区也澄清了:大数值 float32 在 GPU 上不慢(常数时间),慢的是 fp64,我们 shader 不用;引擎已用
> RTC(相对瓦片中心小坐标)+ CPU-double MVP 规避大浮点。
>
> 下文的 float 深度实现细节**仅作技术记录**(离屏 FBO / 两步 MSAA resolve 的正确做法),不代表当前方案。

## 症状

高空(相机远离地表)看整个球体、运动中出现"破碎的缝隙看到后面"(z-fighting 闪烁),
静止愈合,越高越严重,低空无。经真机 `[HOLEDIAG]` 插桩证明**非几何空洞**(每帧
`entries==cmds`,0 missed/missing/kicked)。

## 根因(真机 + 源码定位)

**GLES 后端把 reverse-Z 用在了 24-bit 整数深度上,配固定 `near=150 / far=1e12`。**

- EGL 窗口 surface 深度 = **24-bit 整数**([GLESView.cpp:124](../../scaffold/examples/android/MinimalGlobe/GLESView.cpp) `EGL_DEPTH_SIZE, 24`),主 pass 渲染到**默认帧缓冲**。
- reverse-Z:`glClearDepthf(0)` + `glDepthFunc(GL_GEQUAL)`([RenderDeviceGLES.cpp:474](../../scaffold/src/earth_engine/platform/android/RenderDeviceGLES.cpp))。
- `near=150 / far=1e12`([Scene.cpp:31](../../scaffold/src/earth_engine/scene/Scene.cpp))。

reverse-Z 是**为浮点深度设计**的;配整数深度 + `far/near=1e12/150` 的极端比,7000km 高空时
整个球面只落到 ~150 个整数深度级 → 相邻瓦片/skirt 深度量化相等 → z-fighting。

**关键:Metal 后端已用 `MTLPixelFormatDepth32Float`**([RenderDeviceMetal.mm:542,1021](../../scaffold/src/earth_engine/platform/ios/RenderDeviceMetal.mm)),
所以**此 z-fighting 是 GLES-only**;iOS/Metal 不受影响。

## 成熟引擎基准(.ref 实地调研)

| 引擎 | near/far | 深度技术 |
|---|---|---|
| **Cesium** | 每帧从**实际 command 包围体**算紧 near/far([View.js:311-355](../../.ref/cesium-js/packages/engine/Source/Scene/View.js)) | **对数深度**(默认)+ 多视锥 fallback(`farToNearRatio=1000`) |
| **osgEarth** | `AutoClipPlaneHandler` 每帧夹紧(`nearFarRatio=0.00015`) | **对数深度**([LogDepthBuffer.glsl](../../.ref/osgearth/src/osgEarth/LogDepthBuffer.glsl)) |
| 现代标准 | 可固定 | **Reverse-Z + float32**(精度 ≈ 距离×2⁻²³,与 near/far 无关) |

**从不写死 near/far。** 对数深度是两家为**兼容整数深度 + 老 GLES**吞下的**性能妥协**。

## 性能对比(移动端 early-Z 是关键)

| 方案 | early-Z/Hi-Z | 额外 pass | 移动端代价 |
|---|---|---|---|
| **Reverse-Z + float32** | ✅ 保留 | 无 | 仅深度带宽 D32F vs D24(可忽略) |
| 对数深度 | ❌ **被关**(片元写 `gl_FragDepth`) | 无 | 重(tiler 上 early-Z 是命脉) |
| 多视锥 | ✅ | K pass | 最重(几何重复提交) |

## 决策:方案 A —— Reverse-Z + float32 深度(GLES 离屏 FBO)

**理由**:①我们**已经是 reverse-Z**,当前 bug 本质是"reverse-Z 配了整数深度",补 float32 是它设计时的
正确用法;②**性能最高**(保 early-Z,无额外 pass,无 shader 改动);③**不动 near/far** → 不引入
near 平面 hack 的副作用;④目标机 Adreno 730 (GLES 3.0+) 原生支持 `GL_DEPTH_COMPONENT32F`,不需要
为兼容老硬件用对数深度。

**否决**:近平面 hack(改 near/far 有副作用,见下)、对数深度(移动端性能妥协)、多视锥(多 pass 最重)。

### 实现(GLES-only,自包含在 RenderDeviceGLES;Metal 不动)

EGL 窗口 surface 无法给浮点深度 → 主 pass 渲染到**离屏 FBO**(D32F 深度),再 blit-resolve 颜色到
默认帧缓冲呈现。抽象层 `Framebuffer`/`FramebufferDesc` 已存在但 stub(`createFramebuffer` 返 nullptr),
**无死 RTT 代码**,是新增非复活。全部改动在 GLES 后端内部,`RenderDevice.h` 接口不动。

清单:
1. `RenderDeviceGLES.h`:成员 `sceneFbo_ / sceneColorRb_ / sceneDepthRb_ / fboWidth_/fboHeight_`。
2. `createFbo(w,h,samples)/destroyFbo()`:**多重采样** color RB(`GL_RGBA8`)+ **多重采样 D32F 深度 RB**
   (`glRenderbufferStorageMultisample(GL_RENDERBUFFER, GL_DEPTH_COMPONENT32F, ...)`);`glCheckFramebufferStatus` 守卫。
3. `onSurfaceChanged`:size 变化时 destroy+create(缓存 `fboWidth_/fboHeight_` 防抖动)。
4. `onSurfaceCreated/Destroyed`:随 VAO cache 分配/释放;上下文丢失路径**只丢 CPU 句柄不调 GL**(仿 `dropVaoCache(false)`)。
5. `beginFrame`:首行 `glBindFramebuffer(GL_FRAMEBUFFER, sceneFbo_)`,其余 clear/viewport/depth 契约不变(D32F 仍 `glClearDepthf(0)`)。
6. `endFrame`(现空):`glBlitFramebuffer` 多重采样 FBO → FBO 0(同时 resolve MSAA + 呈现),再 bind 0 供外部 `eglSwapBuffers`。
7. **MSAA 决策**:当前 MSAA 由 EGL 窗口 `EGL_SAMPLES 4` 免费提供 + swap 自动 resolve;一旦离屏,免费 MSAA 没了。
   **推荐**:MSAA 移进离屏 FBO(多重采样 RB + blit resolve),EGL 窗口设 `EGL_SAMPLES 0`。不可静默丢 MSAA(会退化边缘质量)。
8. **Metal 不动**(已 D32F)。跑 `validateMvpRenderCommands` golden 确认深度契约(格式无关)全绿。

契约核查:所有写深度的 pass(terrain/gltf/skybox/atmosphere)只依赖 reverse-Z 的 clear 值 + `GEQUAL`,**与深度位格式无关**;
大气 pass 全屏 quad 在 `clip.z=0`(reverse-Z far)对清零深度做 GEQUAL,D32F 下行为一致(0.0 浮点精确,反而更稳)。无任何深度读回(`glReadPixels` depth / depth-as-texture 均无),故深度附件可用**只写 renderbuffer**(比 texture 便宜、MSAA 友好)。

风险:①MSAA resolve 是主要正确性+性能项(别和窗口 MSAA 叠加双重 resolve);②带宽 D32F 略增(release 真机测,-O2 非 -O0);③resize 重分配防抖动;④上下文丢失丢句柄;⑤`glCheckFramebufferStatus` 守 D32F 支持,log+fallback 防黑屏。

## ⚠️ 次生风险:精度提升会暴露既有"粗/细重叠幽灵"(必须验证)

深挖发现(需真机复验):把深度精度提上去后(**无论近平面 hack 还是 float 深度**),会**暴露一个既有的
粗/细瓦片重叠双绘 bug** —— 高空半透明"幽灵地球"(粗 all-water 瓦片 + sun-glint sheen 叠在细地形上,
带矩形瓦片边界)。机理假说:地形 opaque `GEQUAL` 深度测试下,两个覆盖同像素的表面由深度决定胜者;
低精度(整数+固定 near)时两者量化相等 → 交织融合"看起来干净";精度一提高 → 几何更近的粗瓦片按像素赢 →
细地形从矩形边界"穿透"。sheen 来自 water mask 全瓦片着色([Renderer.cpp:349-368](../../scaffold/src/earth_engine/renderer/Renderer.cpp))。

**但我审 [TileRenderPlanFinalizer.h:60-106](../../scaffold/src/earth_engine/tiling/TileRenderPlanFinalizer.h) 后存疑**:祖先回落是把祖先**裁剪到选中瓦片
footprint**绘制 + 同祖先去重,选中瓦片本身非重叠 → 重叠路径不显然。且 LOD cross-fade(默认关,硬门控
[TileLodTransitionController.h:49-58](../../scaffold/src/earth_engine/tiling/TileLodTransitionController.h))与大气 pass(`hitPlanet` discard)均被排除。

**强可测预言**:因 Metal 已是 float 深度,若幽灵是"精度暴露的既有 bug",**iOS/Metal 应当早已可见此幽灵**。

**落地策略(先验证再修,避免在 golden 敏感的选择/渲染计划上盲改)**:
1. 先只上 **float 深度**(确定性修复 z-fighting)。
2. 真机观察幽灵是否出现:
   - **出现** → 确认是精度暴露的既有重叠 bug,拿到真机复现后再定位/修 `TileRenderPlanFinalizer` 跨祖先空间重叠去重。
   - **不出现** → 幽灵是 near/far-change-specific(近平面 hack 独有),float 深度方案完结。

## 验证(目标驱动)

- 编译:`earth_engine_core` + 全项目 + `native-tests`。
- golden:`test_selector_cesium_golden_diff` + 深度契约 `validateMvpRenderCommands` 相关全绿(格式无关,应零回归)。
- 真机(release/-O2):高空运动破碎消失;两后端(Metal/GLES)视觉一致;帧时不回退。
- 幽灵:见上策略,float 深度落地后真机观察决定是否追加重叠修复。

## 参考文件
- 改:`platform/android/RenderDeviceGLES.cpp/.h`、`examples/android/MinimalGlobe/GLESView.cpp`(EGL samples)。
- 不改(参考):`platform/ios/RenderDeviceMetal.mm`(已 D32F)、`renderer/RenderCommand.cpp`(深度契约)、`renderer/RenderDevice.h`(接口不动)。
- 待验证:`tiling/TileRenderPlanFinalizer.h`、`renderer/Renderer.cpp`(water sheen)。
