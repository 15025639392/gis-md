#pragma once

#include "GpuFrameTiming.h"

#include <memory>
#include <string>
#include <vector>
#include <cstdint>
#include <cstddef>

namespace earth_engine {

// 前向声明
struct TextureDesc;
struct BufferDesc;
struct ShaderDesc;
struct FramebufferDesc;
class Texture;
class Buffer;
class ShaderProgram;
class Framebuffer;
struct RenderCommand;
using RenderCommandList = std::vector<RenderCommand>;

/// 渲染设备抽象接口。
/// 引擎核心通过此接口操作 GPU，不直接依赖 Metal / GL ES / Vulkan API。
/// 平台实现在 platform/ios/ 和 platform/android/ 中。
class RenderDevice {
public:
    virtual ~RenderDevice() = default;

    enum class Backend { Metal, OpenGLES, Vulkan };

    // ---- 能力查询 ----
    virtual Backend backendType() const = 0;
    virtual int maxTextureSize() const = 0;
    virtual int maxDrawBuffers() const = 0;
    virtual bool supportsFloatTextures() const = 0;
    virtual bool supportsInstancing() const = 0;
    /// P6 stencil 分类贴地(方案 B)可用性:后端能执行 VectorStencil 两
    /// phase(需 stencil buffer + 两侧 stencil op)。false → 调用方回落
    /// 方案 A(CPU 高程采样钳制)。默认 false,GLES 覆写 true;Metal 矢量
    /// 路径不出货保持 false。
    virtual bool supportsStencilClassification() const { return false; }
    /// 离屏后处理效果链(passthrough/FXAA/aerial fog)可用性:后端有全屏
    /// shader 接线。默认 true(GL 系 GLSL 已就绪、mock 走空 pass);Metal
    /// 覆写 false(MSL 入口 + submit 侧纹理/uniform 接线待补)。调用方
    /// (Engine 的效果 setter)据此显式拒绝并回报,不再静默 initFailed。
    virtual bool supportsOffscreenPostProcess() const { return true; }
    virtual std::string rendererString() const = 0;

    // ---- 资源创建 ----
    virtual std::unique_ptr<Texture> createTexture(const TextureDesc& desc) = 0;
    /// 把 CPU 像素上传进纹理的一个矩形子区域。
    /// layer:目标数组层(texture2DArray,北极星合成方案页存储 = 一页一层)。
    /// 普通 2D 纹理 layer 必须为 0;数组纹理 layer∈[0, arrayLayers)。
    /// 越界 layer 返回 false(不写)。
    virtual bool updateTextureRegion(Texture* texture,
                                     int x,
                                     int y,
                                     int width,
                                     int height,
                                     const uint8_t* data,
                                     size_t rowBytes,
                                     int layer = 0) = 0;
    virtual std::unique_ptr<Buffer> createBuffer(const BufferDesc& desc) = 0;
    virtual bool updateBuffer(Buffer* buffer,
                              size_t offset,
                              const void* data,
                              size_t size) = 0;
    virtual std::unique_ptr<ShaderProgram> createShader(const ShaderDesc& desc) = 0;
    virtual std::unique_ptr<Framebuffer> createFramebuffer(const FramebufferDesc& desc) = 0;
    /// C-2a:把 framebuffer 的颜色附件改绑到 `target` 的第 `layer` 层
    /// (framebuffer 须以 externalColorTarget 创建)。一个 FBO 逐 draw 改绑即可
    /// 服务全部页,不必每页一个 FBO。
    /// 返回 false = 本后端不支持 / 参数不合法 —— 调用方必须据此短路,**不要**
    /// 当成成功继续画(否则会画到上一次绑定的层上,现象是「内容出现在错误的瓦片」)。
    virtual bool setFramebufferColorLayer(Framebuffer* framebuffer,
                                          Texture* target, int layer) {
        (void)framebuffer;
        (void)target;
        (void)layer;
        return false;
    }

    // ---- 帧操作 ----
    /// 设置后续 beginFrame() 清除颜色缓冲所用的 RGBA（分量 0..1）。
    /// 由引擎在每帧 beginFrame() 前用当前 FrameState 的天空色调用。
    /// 默认实现忽略——离屏 / 测试设备无需清屏色，因此不是纯虚（避免破坏 mock）。
    virtual void setClearColor(float r, float g, float b, float a) {
        (void)r;
        (void)g;
        (void)b;
        (void)a;
    }
    virtual void beginFrame() = 0;
    /// 开启一个渲染 pass。target=nullptr 表示默认目标(屏幕/drawable)。
    /// 每帧须先 beginFrame();pass 不可嵌套;submit() 灌进当前 pass。
    /// 返回 false 表示本 pass 不可用(如 Metal 跳帧),调用方跳过其 submit。
    /// 默认实现 no-op 返回 true——离屏/测试设备无 pass 概念,单 pass 后端
    /// 可继续把 pass-open 逻辑留在 beginFrame(避免破坏 mock)。
    ///
    /// C-2a:`clearTarget=false` 保留目标已有内容(load 而非 clear)。页存储要在
    /// 已灌了底图影像的 array 层上叠画矢量 —— 清掉就把底图抹了。深度/模板同样
    /// 不清(该 pass 是 2D 叠画,不需要它们)。
    virtual bool beginPass(Framebuffer* target, bool clearTarget = true) {
        (void)target;
        (void)clearTarget;
        return true;
    }
    virtual void endPass() {}
    virtual void submit(const RenderCommandList& commands) = 0;
    virtual void endFrame() = 0;

    // ---- GPU 区间计时(测量台;默认关) ----
    // 语义与三条读数边界见 GpuFrameTiming.h。区间**平铺不嵌套**:beginGpuRegion
    // 会先关掉上一个还开着的区间。后端不支持时全部退化为 no-op,调用方无需分支。

    /// 开关。返回开启后的实际状态——扩展缺失/查询对象创建失败时返回 false,
    /// 调用方据此判断"没打开"而不是"打开了但没数"(两者症状相同,必须分开)。
    virtual bool setGpuTimingEnabled(bool /*enabled*/) { return false; }
    virtual bool gpuTimingEnabled() const { return false; }
    /// 计时帧边界。由引擎在 beginFrame() 之后调用并带上真实 frameId ——
    /// 设备自己数帧会和引擎 frameId 漂移(hold 帧引擎计数、设备不计),漂移过的
    /// 帧号贴在 GPU 读数上,比没有帧号更坏。
    virtual void beginGpuFrame(uint64_t /*frameId*/) {}
    /// subdividable=false:抑制 submit() 内部按命令桶的再切分,整段算一个区间。
    /// 深度 prepass 用它 —— 否则 prepass 里的地形命令会和主 pass 的地形命令
    /// 并进同一个 "terrain" 名字,两个性质完全不同的成本被加成一个数。
    virtual void beginGpuRegion(const char* /*name*/, bool /*subdividable*/ = true) {}
    virtual void endGpuRegion() {}
    /// 最近一帧**已完成回读**的结果(滞后数帧,不 stall);无结果时 nullptr。
    virtual const GpuFrameTiming* lastGpuFrameTiming() const { return nullptr; }

    /// GPU→CPU 回读:把离屏 framebuffer 的 color attachment(RGBA8)读进 CPU 缓冲。
    /// 北极星 Phase 2b 虚拟纹理 PoC 的 feedback 通路要量的**固定开销**就在这里——
    /// 移动 GPU 回读需等管线冲刷(glReadPixels / Metal waitUntilCompleted),可能
    /// stall。source=nullptr 或不支持时返回 0。outPixels 需预留 ≥ width*height*4
    /// 字节;返回实际写入字节数(0=失败)。默认 no-op(mock/离屏),两后端各自实现。
    /// 影子渲染自检(方案 C):当前**默认帧缓冲**的降采样指纹。
    ///
    /// 用途:gating 判定 idle 后继续渲 K 帧,指纹若还在变 = 有异步产物落地但
    /// 没人置脏位。这是"画面冻住且零报错"那一类唯一有普适性的守卫 —— 它不
    /// 关心是哪个子系统漏了,只看结果。
    ///
    /// ⚠️ 必须在 swap **之前**调用(swap 后默认帧缓冲内容未定义)。
    /// ⚠️ 内部是**同步回读**,在 TBDR 上是管线 flush(移动端可达毫秒级,不是
    ///    微秒级)。只在 idle 转换那一刻跑,且**严禁在性能测量时开启** ——
    ///    它会污染同一会话里的所有帧时读数。
    /// ⚠️ 只抓得到**上屏**的变化。落地但只影响拾取/碰撞/高度查询的产物,
    ///    画面不变,抓不到 —— 那类要靠 WorkLedger 的账兜。
    ///
    /// 返回 0 = 不支持或失败(调用方据此跳过本次自检,**不得当成"没变化"**);
    /// 否则返回边长 N,outPixels 填 N*N*4 的 RGBA8 降采样。
    ///
    /// 给的是**像素**不是哈希:哈希只答"变没变",而"1 个最低位的噪声"与
    /// "一块瓦片出现了"在哈希上读数完全相同 —— 这类只有二值读数的指标,
    /// 健康态与故障态长得一样,比没有更糟。调用方据此报差异像素数与最大差值。
    virtual int captureFrameSample(std::vector<uint8_t>& /*outPixels*/) {
        return 0;
    }

    virtual size_t readFramebufferPixels(Framebuffer* source,
                                         int x,
                                         int y,
                                         int width,
                                         int height,
                                         uint8_t* outPixels,
                                         size_t outCapacity) {
        (void)source;
        (void)x;
        (void)y;
        (void)width;
        (void)height;
        (void)outPixels;
        (void)outCapacity;
        return 0;
    }

    /// 异步回读(**VT feedback 的生产形态**):发起一次非阻塞的 FBO color 拷贝
    /// (GLES 走 GL_PIXEL_PACK_BUFFER + fence),当帧不 stall;稍后帧用
    /// acquireFramebufferReadback 取回(GPU 已完成则零 stall)。这是公平量「异步
    /// 回读真实 CPU stall」的通路——同步 glReadPixels 强制冲刷不是生产做法。
    /// 返回票号(>0);0 = 不支持(Metal/mock,调用方回落同步)或失败。
    virtual uint64_t enqueueFramebufferReadback(Framebuffer* source,
                                                int x,
                                                int y,
                                                int width,
                                                int height) {
        (void)source;
        (void)x;
        (void)y;
        (void)width;
        (void)height;
        return 0;
    }

    /// 非阻塞取回 enqueue 发起的回读。ticket 就绪则拷进 outPixels、消费该 ticket、
    /// 返回字节数;未就绪返回 0 且 *outStillPending=true;无效票号返回 0 且
    /// *outStillPending=false。**不阻塞**——用 glClientWaitSync(0 超时)轮询 fence,
    /// 故若隔够帧数取,acquire 耗时≈0(这正是要量的)。
    virtual size_t acquireFramebufferReadback(uint64_t ticket,
                                              uint8_t* outPixels,
                                              size_t outCapacity,
                                              bool* outStillPending) {
        (void)ticket;
        (void)outPixels;
        (void)outCapacity;
        if (outStillPending) {
            *outStillPending = false;
        }
        return 0;
    }

    // ---- 生命周期 ----
    /// 渲染 surface 首次创建或 context lost 后重建时调用
    virtual void onSurfaceCreated() = 0;
    /// surface 尺寸变化
    virtual void onSurfaceChanged(int width, int height) = 0;
    /// surface 销毁前调用，释放所有 GPU 资源
    virtual void onSurfaceDestroyed() = 0;
};

// ============================================================
// 资源描述符（桩定义，具体字段后续阶段补充）
// ============================================================

struct TextureDesc {
    int width = 0;
    int height = 0;
    /// 数组层数。1(默认)= 普通 2D 纹理;>1 = texture2DArray(每层独立
    /// CLAMP_TO_EDGE、层间不插值 → 北极星合成方案页存储天然消灭页缝,
    /// 见 §13)。数组纹理创建时不带初始 data,各层经 updateTextureRegion
    /// 的 layer 维分别上传。
    int arrayLayers = 1;
    enum class Format { RGBA8, RGB8, R8, Depth32F } format = Format::RGBA8;
    const uint8_t* data = nullptr;  // 原始像素缓冲区
    size_t dataSize = 0;
    bool mipmap = true;
    enum class Filter { Linear, Nearest } minFilter = Filter::Linear;
    Filter magFilter = Filter::Linear;
    float maxAnisotropy = 1.0f;
    enum class Wrap { Clamp, Repeat, MirroredRepeat } wrapS = Wrap::Clamp, wrapT = Wrap::Clamp;
};

struct BufferDesc {
    size_t size = 0;
    const void* data = nullptr;
    enum class Usage { Static, Dynamic } usage = Usage::Static;
    enum class Type { Vertex, Index, Uniform } type = Type::Vertex;
};

struct ShaderDesc {
    std::string vertexSource;    // MSL / GLSL ES 源码
    std::string fragmentSource;
};

struct FramebufferDesc {
    int width = 0;
    int height = 0;
    bool hasColor = true;
    bool hasDepth = true;
    int samples = 1;
    // depth 是否需被后续 pass 采样(如 aerial fog 从深度重建视距)。
    // false(默认)→ GLES 用 renderbuffer 更省;true → GLES 换深度纹理、
    // Metal 加 ShaderRead usage,经 Framebuffer::depthTexture() 暴露。
    bool depthSampleable = false;
    // P6 stencil 分类需要:深度附件带 8 位 stencil(GLES 用
    // DEPTH32F_STENCIL8,深度精度不变)。⚠️ 无 stencil 附件的
    // framebuffer 上 stencil 测试按 GL 规范恒通过 → VectorStencil
    // 分类静默失效(真机踩过:离屏场景 pass 没 stencil,整个体积侧影
    // 被染色)。场景主 pass 的离屏目标必须开。
    bool hasStencil = false;
    // C-2a:把**已有 texture2DArray 的某一层**当作颜色附件,而不是自建颜色纹理。
    // 页存储要在自己的 array 层上直接画矢量(RTT draping)——不这么做就只能画进
    // scratch FBO 再拷一次(GLES glCopyTexSubImage3D / Metal blit),白搭一次带宽。
    // 非空时 hasColor 的自建路径被跳过,colorTexture() 返回该外部纹理(不持有)。
    // 层可经 RenderDevice::setFramebufferColorLayer 逐 draw 改绑 —— 一个 FBO 服务
    // 全部页,不必每页一个 FBO。
    Texture* externalColorTarget = nullptr;
    int externalColorLayer = 0;
};

// ============================================================
// GPU 资源抽象基类
// ============================================================

class Texture {
public:
    virtual ~Texture() = default;
    virtual int width() const = 0;
    virtual int height() const = 0;
    /// Bytes allocated for this texture, including allocated mip levels.
    virtual size_t sizeBytes() const = 0;
};

class Buffer {
public:
    virtual ~Buffer() = default;
    virtual size_t size() const = 0;
};

class ShaderProgram {
public:
    virtual ~ShaderProgram() = default;
};

class Framebuffer {
public:
    virtual ~Framebuffer() = default;
    virtual int width() const = 0;
    virtual int height() const = 0;
    /// 离屏 color attachment,恒为可采样纹理:可直接塞进
    /// RenderCommand::textures 被后续 pass 采样。生命周期归 Framebuffer。
    /// v 轴方向保持各后端原生方向(GL bottom-left / Metal top-left),
    /// 消费它的全屏采样 shader 按后端各自写对 UV,引擎层不做 flip。
    virtual Texture* colorTexture() const = 0;
    /// 离屏 depth attachment 作可采样纹理:仅当 FramebufferDesc.depthSampleable
    /// 时非空(否则 depth 是 renderbuffer,返回 nullptr)。采样值为 window
    /// depth [0,1](reverse-Z:近=1、远→0.5、背景=0),消费方自行线性化视距。
    virtual Texture* depthTexture() const { return nullptr; }
};

} // namespace earth_engine
