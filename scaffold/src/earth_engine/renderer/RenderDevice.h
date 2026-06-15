#pragma once

#include <memory>
#include <string>
#include <vector>
#include <cstdint>

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
    virtual bool updateTextureRegion(Texture* texture,
                                     int x,
                                     int y,
                                     int width,
                                     int height,
                                     const uint8_t* data,
                                     size_t rowBytes) = 0;
    virtual std::unique_ptr<Buffer> createBuffer(const BufferDesc& desc) = 0;
    virtual bool updateBuffer(Buffer* buffer,
                              size_t offset,
                              const void* data,
                              size_t size) = 0;
    virtual std::unique_ptr<ShaderProgram> createShader(const ShaderDesc& desc) = 0;
    virtual std::unique_ptr<Framebuffer> createFramebuffer(const FramebufferDesc& desc) = 0;

    // ---- 帧操作 ----
    virtual void beginFrame() = 0;
    virtual void submit(const RenderCommandList& commands) = 0;
    virtual void endFrame() = 0;

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
};

// ============================================================
// GPU 资源抽象基类
// ============================================================

class Texture {
public:
    virtual ~Texture() = default;
    virtual int width() const = 0;
    virtual int height() const = 0;
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
};

} // namespace earth_engine
