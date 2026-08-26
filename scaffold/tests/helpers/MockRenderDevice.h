#pragma once

#include "earth_engine/renderer/RenderDevice.h"
#include "earth_engine/renderer/RenderCommand.h"  // RenderCommandList 成员需完整类型

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace earth_engine {
namespace testing {

// ============================================================
// 共享 mock 对象，在 macOS 上模拟平台层行为。
// 每个测试文件不再需要各自内联定义。
// ============================================================

class DummyShaderProgram final : public ShaderProgram {};

class DummyTexture final : public Texture {
public:
    DummyTexture(int width, int height) : width_(width), height_(height) {}
    int width() const override { return width_; }
    int height() const override { return height_; }
    size_t sizeBytes() const override {
        return static_cast<size_t>(width_) *
               static_cast<size_t>(height_) * 4u;
    }
private:
    int width_ = 0, height_ = 0;
};

class DummyFramebuffer final : public Framebuffer {
public:
    DummyFramebuffer(int width, int height)
        : width_(width), height_(height),
          colorTexture_(std::make_unique<DummyTexture>(width, height)) {}
    int width() const override { return width_; }
    int height() const override { return height_; }
    Texture* colorTexture() const override { return colorTexture_.get(); }
private:
    int width_ = 0, height_ = 0;
    std::unique_ptr<DummyTexture> colorTexture_;
};

class DummyBuffer final : public Buffer {
public:
    explicit DummyBuffer(size_t byteSize) : byteSize_(byteSize) {}
    DummyBuffer(size_t byteSize, const void* data) : byteSize_(byteSize) {
        if (data && byteSize > 0) {
            const auto* begin = static_cast<const uint8_t*>(data);
            bytes_.assign(begin, begin + byteSize);
        }
    }
    size_t size() const override { return byteSize_; }
    const std::vector<uint8_t>& bytes() const { return bytes_; }
    bool update(size_t offset, const void* data, size_t size) {
        if (!data || offset > byteSize_ || size > byteSize_ - offset) {
            return false;
        }
        if (bytes_.size() != byteSize_) {
            bytes_.resize(byteSize_);
        }
        const auto* begin = static_cast<const uint8_t*>(data);
        std::copy(begin, begin + size, bytes_.begin() + offset);
        return true;
    }
private:
    size_t byteSize_ = 0;
    std::vector<uint8_t> bytes_;
};

/**
 * @brief 完整的 RenderDevice mock，记录所有 GPU 调用。
 *
 * 用于在 macOS 上验证 RenderCommand 产生是否正确，
 * 无需真实的 GL ES 上下文。
 */
class MockRenderDevice final : public RenderDevice {
public:
    Backend backendType() const override { return Backend::OpenGLES; }
    int maxTextureSize() const override { return 4096; }
    int maxDrawBuffers() const override { return 4; }
    bool supportsFloatTextures() const override { return true; }
    bool supportsInstancing() const override { return true; }
    // P6 stencil 分类:默认支持,测试可关(验证回落方案 A 路径)。
    bool supportsStencilClassification() const override {
        return stencilClassificationSupported;
    }
    bool stencilClassificationSupported = true;
    std::string rendererString() const override { return "MockRenderDevice"; }

    std::unique_ptr<Texture> createTexture(const TextureDesc& desc) override {
        if (!allowTextureCreation) return nullptr;
        lastTextureDesc = desc;
        ++createdTextureCount;
        return std::make_unique<DummyTexture>(desc.width, desc.height);
    }

    bool updateTextureRegion(Texture*, int, int, int, int,
                             const uint8_t*, size_t, int = 0) override {
        ++textureRegionUpdateCount;
        return textureRegionUploadSucceeds;
    }
    // H-S4:批量上传计数(测试边缘 LUT 批处理:多层应合并成一次调用)。
    int textureArrayRegionUpdateCount = 0;
    int lastBatchFirstLayer = -1;
    int lastBatchLayerCount = 0;
    bool updateTextureArrayRegion(Texture*, int, int, int, int,
                                  int firstLayer, int layerCount,
                                  const uint8_t*, size_t) override {
        ++textureArrayRegionUpdateCount;
        lastBatchFirstLayer = firstLayer;
        lastBatchLayerCount = layerCount;
        return textureRegionUploadSucceeds;
    }

    std::unique_ptr<Buffer> createBuffer(const BufferDesc& desc) override {
        ++bufferCreationAttempts;
        if (failBufferCreationAtAttempt == bufferCreationAttempts) {
            return nullptr;
        }
        auto buf = std::make_unique<DummyBuffer>(desc.size, desc.data);
        ++createdBufferCount;
        return buf;
    }

    bool updateBuffer(Buffer* buffer,
                      size_t offset,
                      const void* data,
                      size_t size) override {
        if (!allowBufferUpdates) return false;
        auto* dummy = dynamic_cast<DummyBuffer*>(buffer);
        if (!dummy || !dummy->update(offset, data, size)) {
            return false;
        }
        ++updatedBufferCount;
        lastBufferUpdateOffset = offset;
        lastBufferUpdateSize = size;
        totalBufferUpdateBytes += size;
        return true;
    }

    std::unique_ptr<ShaderProgram> createShader(const ShaderDesc&) override {
        ++shaderCount;
        return std::make_unique<DummyShaderProgram>();
    }

    std::unique_ptr<Framebuffer> createFramebuffer(const FramebufferDesc& desc) override {
        if (!allowFramebufferCreation) return nullptr;
        ++createdFramebufferCount;
        return std::make_unique<DummyFramebuffer>(desc.width, desc.height);
    }

    bool setFramebufferColorLayer(Framebuffer*, Texture*, int layer) override {
        ++colorLayerRebindCount;
        lastColorLayerBound = layer;
        return framebufferColorLayerRebindSucceeds;
    }

    size_t readFramebufferPixels(Framebuffer*,
                                 int,
                                 int,
                                 int width,
                                 int height,
                                 uint8_t* outPixels,
                                 size_t outCapacity) override {
        ++readbackCount;
        const size_t needed =
            static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
        if (!outPixels || outCapacity < needed) {
            return 0;
        }
        // 用测试预置的 canned 像素填充(平铺/截断到请求尺寸);未设置则清 0(全背景)。
        if (cannedFeedbackPixels.empty()) {
            std::fill(outPixels, outPixels + needed, uint8_t{0});
        } else {
            for (size_t i = 0; i < needed; ++i) {
                outPixels[i] =
                    cannedFeedbackPixels[i % cannedFeedbackPixels.size()];
            }
        }
        return needed;
    }

    uint64_t enqueueFramebufferReadback(Framebuffer*,
                                        int,
                                        int,
                                        int width,
                                        int height) override {
        if (!supportsAsyncReadback) return 0;
        ++enqueueCount;
        lastEnqueueBytes =
            static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
        return nextTicket_++;
    }
    size_t acquireFramebufferReadback(uint64_t ticket,
                                      uint8_t* outPixels,
                                      size_t outCapacity,
                                      bool* outStillPending) override {
        if (outStillPending) *outStillPending = false;
        if (ticket == 0 || !outPixels) return 0;
        ++acquireCount;
        if (asyncAlwaysPending) {
            if (outStillPending) *outStillPending = true;
            return 0;
        }
        const size_t needed = lastEnqueueBytes;
        if (outCapacity < needed) return 0;
        if (cannedFeedbackPixels.empty()) {
            std::fill(outPixels, outPixels + needed, uint8_t{0});
        } else {
            for (size_t i = 0; i < needed; ++i) {
                outPixels[i] =
                    cannedFeedbackPixels[i % cannedFeedbackPixels.size()];
            }
        }
        return needed;
    }

    void beginFrame() override { ++frameCount; }
    bool beginPass(Framebuffer* target, bool clearTarget = true) override {
        ++beginPassCount;
        lastPassTarget = target;
        return true;
    }
    void endPass() override { ++endPassCount; }
    void submit(const RenderCommandList& commands) override {
        submittedCommands = commands;
        ++submitCount;
    }
    void endFrame() override {}
    void onSurfaceCreated() override {}
    void onSurfaceChanged(int, int) override {}
    void onSurfaceDestroyed() override {}

    // --- 记录字段 ---
    RenderCommandList submittedCommands;
    TextureDesc lastTextureDesc;
    int createdTextureCount = 0;
    int textureRegionUpdateCount = 0;
    /// region 上传结果。默认 false 是历史行为(多数用例只关心是否发起
    /// 上传);把上传成败当契约的用例(如 IconAtlas 注册 frame)置 true。
    bool textureRegionUploadSucceeds = false;
    int createdBufferCount = 0;
    int updatedBufferCount = 0;
    size_t lastBufferUpdateOffset = 0;
    size_t lastBufferUpdateSize = 0;
    size_t totalBufferUpdateBytes = 0;
    int bufferCreationAttempts = 0;
    int failBufferCreationAtAttempt = -1;
    int shaderCount = 0;
    int submitCount = 0;
    int frameCount = 0;
    int createdFramebufferCount = 0;
    int readbackCount = 0;
    int enqueueCount = 0;
    int acquireCount = 0;
    size_t lastEnqueueBytes = 0;
    int beginPassCount = 0;
    int endPassCount = 0;
    int colorLayerRebindCount = 0;
    int lastColorLayerBound = -1;
    /// 层改绑结果。默认 true(生产 GLES/Metal 都支持);测「不支持后端的
    /// 回落路径」的用例置 false。
    bool framebufferColorLayerRebindSucceeds = true;
    Framebuffer* lastPassTarget = nullptr;
    // 测试预置的 feedback 像素(RGBA8),readFramebufferPixels 平铺回填之。
    std::vector<uint8_t> cannedFeedbackPixels;
    bool allowTextureCreation = true;
    bool allowBufferUpdates = true;
    bool allowFramebufferCreation = true;
    bool supportsAsyncReadback = false;  // true → 走异步 enqueue/acquire 路径
    bool asyncAlwaysPending = false;     // true → acquire 恒未就绪(测延迟路径)

private:
    uint64_t nextTicket_ = 1;
};

} // namespace testing
} // namespace earth_engine
