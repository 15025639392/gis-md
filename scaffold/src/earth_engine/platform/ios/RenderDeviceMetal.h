#pragma once

#include "../../renderer/RenderDevice.h"

namespace earth_engine {

/// Metal 2 渲染后端（iOS / macOS）。
/// 通过 CAMetalLayer 获取 drawable，每帧提交 MTLCommandBuffer。
class RenderDeviceMetal : public RenderDevice {
public:
    /// @param metalLayer CAMetalLayer*（void* 避免头文件依赖 Metal.h）
    explicit RenderDeviceMetal(void* metalLayer);
    ~RenderDeviceMetal() override;

    // ---- 能力查询 ----
    Backend backendType() const override { return Backend::Metal; }
    int maxTextureSize() const override;
    int maxDrawBuffers() const override;
    bool supportsFloatTextures() const override;
    bool supportsInstancing() const override;
    // 全屏后处理 shader 仅有 GLSL,MSL 接线待补(见 OffscreenPostProcess.h)。
    bool supportsOffscreenPostProcess() const override { return false; }
    std::string rendererString() const override;

    // ---- 资源创建 ----
    std::unique_ptr<Texture> createTexture(const TextureDesc& desc) override;
    bool updateTextureRegion(Texture* texture,
                             int x,
                             int y,
                             int width,
                             int height,
                             const uint8_t* data,
                             size_t rowBytes,
                             int layer = 0) override;
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
    bool beginPass(Framebuffer* target) override;
    void endPass() override;
    void submit(const RenderCommandList& commands) override;
    void endFrame() override;
    size_t readFramebufferPixels(Framebuffer* source,
                                 int x,
                                 int y,
                                 int width,
                                 int height,
                                 uint8_t* outPixels,
                                 size_t outCapacity) override;

    // ---- 生命周期 ----
    void onSurfaceCreated() override;
    void onSurfaceChanged(int width, int height) override;
    void onSurfaceDestroyed() override;

private:
    struct Impl;
    Impl* impl_;  // PImpl，在 .mm 中定义
};

} // namespace earth_engine
