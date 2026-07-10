# 离屏渲染通路(createFramebuffer + pass 抽象)设计 — 2026-07-10

## 0. 背景与目标

`createFramebuffer` 在两后端均为一行 `return nullptr`(RenderDeviceMetal.mm:572 / RenderDeviceGLES.cpp:446,注释"MVP 不需要"),这是引擎观感/功能天花板的 **#1 唯一 major 阻断**:AA、HDR/tone-mapping、bloom、阴影(CSM)、地表大气所需离屏、RTT/viewshed、拾取缓冲全部被焊死。

调研发现缺口比"补实现"大一圈,三个事实:

1. **引擎侧零调用者** —— scene/renderer 无任何代码调用 createFramebuffer(仅测试 mock override)。缺的不只是实现,是"这批命令画到哪"的**pass 概念**。
2. **帧模型是硬编码单 pass** —— Metal `beginFrame()` 直接拿 drawable 建唯一 encoder(pass 描述/clear/reverse-Z/viewport 全绑死),`submit()` 只往这个 encoder 灌命令;GLES `beginFrame()` 隐式画到默认 framebuffer。
3. **两个必须继承的既有契约** —— ①reverse-Z(clear depth=0,GEQUAL/对应 Metal 深度状态);②winding 契约(Metal CW / GLES CCW 故意相反,补偿 y 原点差,勿统一)。

**目标**:补齐"Framebuffer 资源 + 显式 pass API",并用一个最小下游(RTT passthrough:场景画进离屏纹理再全屏 blit 上屏)点亮验证。默认 OFF,零回归。

## 1. 成熟引擎基准(.ref 源码调研,两个 agent 各自带 file:line 证据)

| 引擎 | 可抄的概念 |
|---|---|
| **cesium-js** | `Framebuffer.js` 封装多 attachment+互斥完整性检查;**per-command framebuffer 覆盖 pass 默认值**(Context.js:1412 `drawCommand._framebuffer ?? passState.framebuffer`);`FramebufferManager` 尺寸/采样**脏检测→销毁重建**(无 resize);后处理 ping-pong 由 `PostProcessStageTextureCache` 池化;最小消费者=`PickFramebuffer`(一个 manager+一个 PassState,画完 readPixels)。深度走 packed depth-stencil texture + `czm_packDepth` 拷贝,**log-depth 非 reverse-Z**(与我们不同,不抄深度约定)。 |
| **maplibre-gl-js** | 最干净的接口形态:**薄 FBO holder**(framebuffer.ts:9-45)+ **attachment=一等可采样对象**(color 恒为 texture 可直接 bindTexture 采样,depth 用 renderbuffer,value.ts:488-526);pass 切换=`context.bindFramebuffer.set(fbo/null)` 带 dirty 去重;**尺寸变化=记尺寸+惰性 destroy 重建**(terrain.ts:318),resize 不主动重建。 |
| **osgearth** | C++ 侧生命周期契约:`GLObject::release()`/`resizeGLObjectBuffers`/`releaseGLObjects` + `GLObjectPool` 帧延迟回收(30 帧/每帧字节配额,GLUtils.cpp:531-742),防删仍在管线中的资源;`GLFBO::renderToTexture` **返回可采样 GLTexture**(GLUtils.cpp:1794)。教训:它自研的 GLFBO 无调用者——**FBO 抽象必须与主渲染循环打通才有意义**(正是我们要做的 pass API)。 |

## 2. 设计

### 2.1 Framebuffer 资源(抄 maplibre:attachment=可采样一等 Texture)

`RenderDevice.h` 的 `Framebuffer` 基类从空壳扩为:

```cpp
class Framebuffer {
public:
    virtual ~Framebuffer() = default;
    virtual int width() const = 0;
    virtual int height() const = 0;
    /// 离屏 color attachment,恒为可采样纹理:可直接塞进
    /// RenderCommand::textures 被后续 pass 采样。生命周期归 Framebuffer。
    virtual Texture* colorTexture() const = 0;
};
```

`FramebufferDesc` 现有 `{width,height,hasColor,hasDepth,samples}` 够 v1 用;**v1 约束 samples=1**(desc.samples>1 时按 1 处理并 Warning,MSAA resolve 是 F4+ 的事,cesium 的 MultisampleFramebuffer blit 模式留作参考)。

**GLES 实现**(GLFramebuffer):`glGenFramebuffers`;color=GL_RGBA8 texture(`glFramebufferTexture2D` COLOR_ATTACHMENT0,NEAREST/CLAMP);depth=`GL_DEPTH_COMPONENT32F` renderbuffer(ES3 保证,匹配 reverse-Z 精度需求;不需被采样时 renderbuffer 更省——maplibre 同款选择);`glCheckFramebufferStatus != COMPLETE` → 释放已建资源返回 nullptr + platformLog Warning。析构 `glDeleteFramebuffers/Textures/Renderbuffers`(GL 语义内部延迟,安全)。

**Metal 实现**(MetalFramebuffer):color=MTLTexture(`MTLPixelFormatBGRA8Unorm`,usage=RenderTarget|ShaderRead);depth=MTLTexture `Depth32Float`(usage=RenderTarget,storageMode=Private,与主 pass 深度同格式);不预建 pass descriptor——**descriptor 是 beginPass 的事**(每 pass 一个)。析构安全:Metal command buffer 默认 retain 引用资源,无需 osgearth 式延迟回收池(GLES 同理)——**v1 不建回收池**,依据写进注释。

### 2.2 Pass API(显式 begin/end,不抄 cesium 的 per-command 覆盖)

cesium 的 per-command framebuffer 是 WebGL 惯用法;对 Metal(pass=encoder,一次成型)per-command 切换意味着任意 draw 边界撕 encoder,与常驻命令缓存(resident draw command cache)交互复杂。**取 pass-scoped 显式 API**(基准调研同样建议:把 encoder 生命周期藏进 begin/end):

```cpp
// RenderDevice 新增(带默认 no-op 实现,mock/测试零改动):
/// 开启一个渲染 pass。target=nullptr 表示默认目标(屏幕/drawable)。
/// 每帧必须先 beginFrame();pass 不可嵌套;submit() 灌进当前 pass。
virtual bool beginPass(Framebuffer* target) { (void)target; return true; }
virtual void endPass() {}
```

**帧模型重构**(唯一生产编排点 Engine.cpp:87-101):

```
beginFrame()            // 只做帧获取:Metal=信号量+command buffer+drawable;GLES=viewport 记录
beginPass(offscreen)    // (可选,0..N 个)离屏 pass:clear+状态设置,画到 FBO
  submit(...)
endPass()
beginPass(nullptr)      // 主 pass:即现有 beginFrame 里的 pass-open 逻辑原样搬入
  submit(commands)
endPass()
endFrame()              // Metal=present+commit;GLES=不变(外部 eglSwapBuffers)
```

- Metal:beginFrame 保留信号量闸门/超时跳帧/drawable 获取;pass-open(RenderPassDescriptor、clear、reverse-Z clearDepth=0、viewport、CW winding)逐字搬进 `beginPass`,target≠nullptr 时 attachment 换成 FBO 纹理、viewport 用 FBO 尺寸。encoder 串行:endPass=endEncoding。跳帧时(commandBuffer==nil)beginPass 返回 false,submit 已有 encoder 空判。
- GLES:beginPass(nullptr)=现 beginFrame 的 clear+状态块;target≠nullptr 时 `glBindFramebuffer(fbo)`+glViewport(FBO 尺寸)+同样的 clear/reverse-Z/CCW 状态;endPass 回绑 0 号 framebuffer 并恢复主 viewport。
- **每个 pass 内两侧 y 方向/winding 与各自 onscreen 一致 → winding 契约不动**。
- 兼容:default no-op 虚函数 → 全部测试 mock、离屏测试设备零改动;native 套件不受影响。

### 2.3 y-flip / UV 契约(跨后端 RTT 经典坑,显式定死)

采样离屏纹理时:GL 纹理原点 bottom-left,Metal top-left → 同一 UV 两侧竖直互翻。**契约:离屏 colorTexture 的 v 轴方向保持各后端原生方向;消费它的全屏采样 shader 属于后端 shader 源(本就 MSL/GLSL 分写),各自写对 UV,不在引擎层引入 flip uniform。** 验证靠 F3 的截图对比(blit ON≈OFF),翻了立刻可见(画面上下颠倒)。

### 2.4 尺寸变化(抄 maplibre 惰性策略)

Framebuffer 无 resize 方法。持有者(v1=RTT passthrough 路径)缓存 FBO,每帧比对 `width/height` 与当前 viewport,不符则析构重建(cesium FramebufferManager 的脏检测同款,池化管理器等首个真后处理消费者落地时再建,**不预做**)。`onSurfaceDestroyed` 时持有者释放 FBO(GLES context lost 后 GL 名字失效,重建走同一惰性路径)。

## 3. 分期落地(目标驱动,每步可验证)

**分支**:椭球线在 `feat/ellipsoid-fallback-composite-provider` 已自洽,本工作从 main 新开 `feat/offscreen-render-pass`。

- **F1 Framebuffer 资源**:扩 Framebuffer 接口 + 两后端 createFramebuffer 真实现。验证:真机日志 `[RTTDIAG] fbo complete WxH`,glError=0;native 全绿(mock 不受影响)。
- **F2 pass API 重构**:beginPass/endPass + Engine.cpp 接线,pass-open 逻辑从 beginFrame 原样搬移。验证:**基线不变**——真机 flag-OFF 画面与重构前一致(同场景截图对比)+ native 全绿;这是纯等价重构闸。
- **F3 RTT passthrough 点亮(flag-gated,默认 OFF)**:场景画进离屏 FBO → 全屏 blit 上屏(blit shader 循既有全屏 atmosphere pass 的形态,MSL/GLSL 各写)。机制信号(我验):`[RTTDIAG] offscreenPass=1 blitDrawn=1 glError=0`;像素判断(客观,可 adb 截图对比):flag ON vs OFF 截图近似逐像素一致(允许 8bit 舍入);场景钉死(demo 初始相机,ion 地形收敛后)。
- **F4+(不预做)**:MSAA resolve、FramebufferManager 式池化、深度纹理暴露给后处理——等第一个真消费者(FXAA/aerial fog/拾取)选定时按需落地。

## 4. 风险与已知取舍

- **F2 是重构最重的一步**:beginFrame 拆分若漏搬一行状态(如 GLES 的 depthMask 恢复)会出"穿孔地表"类回归——搬移必须逐字,靠 flag-OFF 截图对比闸兜底。
- **Metal 跳帧路径**:信号量超时/drawable 为 nil 时 beginPass 必须安全返回 false,消费者跳过本帧离屏工作。
- **samples>1 静默降 1**:v1 明确不支持 MSAA,Warning 提示,不假装支持。
- **不建回收池**:两后端删除语义本身安全(Metal retain/GL 内部延迟);若未来上 Vulkan 后端才需要 osgearth 式延迟回收,届时再建。
- **常驻命令缓存不受影响**:pass 归属由 submit 调用点(编排层)决定,RenderCommand 结构零改动。
