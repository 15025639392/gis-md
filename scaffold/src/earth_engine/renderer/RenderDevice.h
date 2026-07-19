#pragma once

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
    virtual bool beginPass(Framebuffer* target) {
        (void)target;
        return true;
    }
    virtual void endPass() {}
    virtual void submit(const RenderCommandList& commands) = 0;
    virtual void endFrame() = 0;

    /// GPU→CPU 回读:把离屏 framebuffer 的 color attachment(RGBA8)读进 CPU 缓冲。
    /// 北极星 Phase 2b 虚拟纹理 PoC 的 feedback 通路要量的**固定开销**就在这里——
    /// 移动 GPU 回读需等管线冲刷(glReadPixels / Metal waitUntilCompleted),可能
    /// stall。source=nullptr 或不支持时返回 0。outPixels 需预留 ≥ width*height*4
    /// 字节;返回实际写入字节数(0=失败)。默认 no-op(mock/离屏),两后端各自实现。
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
