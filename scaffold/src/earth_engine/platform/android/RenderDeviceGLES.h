#pragma once

#include "../../renderer/RenderDevice.h"
#include <unordered_map>
#include <vector>
#include <string>

namespace earth_engine {

/// OpenGL ES 3.0 渲染后端。
/// 假设调用者已创建并激活 EGL context。
class RenderDeviceGLES : public RenderDevice {
public:
    RenderDeviceGLES();
    ~RenderDeviceGLES() override;

    // ---- 能力查询 ----
    Backend backendType() const override { return Backend::OpenGLES; }
    int maxTextureSize() const override;
    int maxDrawBuffers() const override;
    bool supportsFloatTextures() const override;
    bool supportsInstancing() const override;
    std::string rendererString() const override;

    // ---- 资源创建 ----
    std::unique_ptr<Texture> createTexture(const TextureDesc& desc) override;
    bool updateTextureRegion(Texture* texture,
                             int x,
                             int y,
                             int width,
                             int height,
                             const uint8_t* data,
                             size_t rowBytes) override;
    std::unique_ptr<Buffer> createBuffer(const BufferDesc& desc) override;
    bool updateBuffer(Buffer* buffer,
                      size_t offset,
                      const void* data,
                      size_t size) override;
    std::unique_ptr<ShaderProgram> createShader(const ShaderDesc& desc) override;
    std::unique_ptr<Framebuffer> createFramebuffer(const FramebufferDesc& desc) override;

    // ---- 帧操作 ----
    void setClearColor(float r, float g, float b, float a) override;
    void beginFrame() override;
    void submit(const RenderCommandList& commands) override;
    void endFrame() override;

    // ---- 生命周期 ----
    void onSurfaceCreated() override;
    void onSurfaceChanged(int width, int height) override;
    void onSurfaceDestroyed() override;

private:
    int viewportWidth_ = 0;
    int viewportHeight_ = 0;
    // Sky clear color pushed by Engine each frame via setClearColor().
    // Defaults match FrameState (dark night blue) until the first update.
    float clearR_ = 0.02f;
    float clearG_ = 0.02f;
    float clearB_ = 0.08f;
    float clearA_ = 1.0f;
};

// ============================================================
// 内部 GL 资源类型
// ============================================================

class GLTexture : public Texture {
public:
    GLTexture(unsigned int id, int width, int height);
    ~GLTexture() override;
    int width() const override { return width_; }
    int height() const override { return height_; }
    unsigned int glId() const { return id_; }
private:
    unsigned int id_;
    int width_, height_;
};

class GLBuffer : public Buffer {
public:
    GLBuffer(unsigned int id, size_t size, unsigned int target);
    ~GLBuffer() override;
    size_t size() const override { return size_; }
    unsigned int glId() const { return id_; }
    unsigned int target() const { return target_; }
private:
    unsigned int id_;
    size_t size_;
    unsigned int target_;  // GL_ARRAY_BUFFER / GL_ELEMENT_ARRAY_BUFFER
};

class GLShaderProgram : public ShaderProgram {
public:
    GLShaderProgram(unsigned int programId);
    ~GLShaderProgram() override;
    unsigned int glId() const { return id_; }

    /// 获取 uniform location（缓存）
    int uniformLocation(const std::string& name);

private:
    unsigned int id_;
    std::unordered_map<std::string, int> uniformCache_;
};

} // namespace earth_engine
