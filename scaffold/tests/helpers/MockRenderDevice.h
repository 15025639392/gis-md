#pragma once

#include "earth_engine/renderer/RenderDevice.h"

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
    std::string rendererString() const override { return "MockRenderDevice"; }

    std::unique_ptr<Texture> createTexture(const TextureDesc& desc) override {
        if (!allowTextureCreation) return nullptr;
        lastTextureDesc = desc;
        ++createdTextureCount;
        return std::make_unique<DummyTexture>(desc.width, desc.height);
    }

    bool updateTextureRegion(Texture*, int, int, int, int,
                             const uint8_t*, size_t) override {
        return false;
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
        return true;
    }

    std::unique_ptr<ShaderProgram> createShader(const ShaderDesc&) override {
        ++shaderCount;
        return std::make_unique<DummyShaderProgram>();
    }

    std::unique_ptr<Framebuffer> createFramebuffer(const FramebufferDesc&) override {
        return nullptr;
    }

    void beginFrame() override { ++frameCount; }
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
    int createdBufferCount = 0;
    int updatedBufferCount = 0;
    int bufferCreationAttempts = 0;
    int failBufferCreationAtAttempt = -1;
    int shaderCount = 0;
    int submitCount = 0;
    int frameCount = 0;
    bool allowTextureCreation = true;
    bool allowBufferUpdates = true;
};

} // namespace testing
} // namespace earth_engine
