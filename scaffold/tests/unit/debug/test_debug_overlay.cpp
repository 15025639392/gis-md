#include <gtest/gtest.h>

#include "earth_engine/debug/DebugOverlay.h"
#include "earth_engine/renderer/RenderDevice.h"
#include "earth_engine/tiling/TileScheme.h"

using namespace earth_engine;

namespace {

class FakeBuffer : public Buffer {
public:
    explicit FakeBuffer(size_t size) : size_(size) {}
    size_t size() const override { return size_; }

private:
    size_t size_;
};

class FakeShaderProgram : public ShaderProgram {};

class FakeRenderDevice : public RenderDevice {
public:
    Backend backendType() const override { return Backend::OpenGLES; }
    int maxTextureSize() const override { return 4096; }
    int maxDrawBuffers() const override { return 4; }
    bool supportsFloatTextures() const override { return true; }
    bool supportsInstancing() const override { return true; }
    std::string rendererString() const override { return "fake"; }

    std::unique_ptr<Texture> createTexture(const TextureDesc&) override {
        return nullptr;
    }

    std::unique_ptr<Buffer> createBuffer(const BufferDesc& desc) override {
        return std::make_unique<FakeBuffer>(desc.size);
    }

    bool updateBuffer(Buffer* buffer,
                      size_t offset,
                      const void* data,
                      size_t size) override {
        return buffer && data && offset + size <= buffer->size();
    }

    std::unique_ptr<ShaderProgram> createShader(const ShaderDesc&) override {
        return std::make_unique<FakeShaderProgram>();
    }

    std::unique_ptr<Framebuffer> createFramebuffer(const FramebufferDesc&) override {
        return nullptr;
    }

    void beginFrame() override {}
    void submit(const RenderCommandList&) override {}
    void endFrame() override {}
    void onSurfaceCreated() override {}
    void onSurfaceChanged(int, int) override {}
    void onSurfaceDestroyed() override {}
};

} // namespace

TEST(DebugOverlayTest, LodTransitionStateOverridesExactTileColor) {
    FakeRenderDevice device;
    DebugOverlay overlay;
    ASSERT_TRUE(overlay.initialize(&device));

    LayerTilePlan plan;
    TileKey key{"XYZ-WebMercator", 3, 4, 3};
    plan.visibleTiles = {key};
    plan.desiredTiles = {key};
    plan.renderTiles.push_back(RenderTileRef{
        key, key, TileRenderSource::Exact, TileReadinessState::Ready, 0.35f});
    plan.tileTransitions.push_back(TileTransition{key, 0.35f, 1});

    auto scheme = TileScheme::createXYZWebMercator();
    RenderCommandList commands;
    overlay.buildCommands(plan, *scheme, commands);

    ASSERT_EQ(1u, commands.size());
    const auto& color = commands[0].uniforms.at("u_color");
    ASSERT_EQ(4u, color.size());
    EXPECT_FLOAT_EQ(0.82f, color[0]);
    EXPECT_FLOAT_EQ(0.32f, color[1]);
    EXPECT_FLOAT_EQ(1.0f, color[2]);
    EXPECT_FLOAT_EQ(0.9f, color[3]);
}

TEST(DebugOverlayTest, ExactTileColorRemainsGreenWithoutLodTransition) {
    FakeRenderDevice device;
    DebugOverlay overlay;
    ASSERT_TRUE(overlay.initialize(&device));

    LayerTilePlan plan;
    TileKey key{"XYZ-WebMercator", 3, 4, 3};
    plan.visibleTiles = {key};
    plan.desiredTiles = {key};
    plan.renderTiles.push_back(RenderTileRef{
        key, key, TileRenderSource::Exact, TileReadinessState::Ready, 1.0f});
    plan.tileTransitions.push_back(TileTransition{key, 1.0f, 0});

    auto scheme = TileScheme::createXYZWebMercator();
    RenderCommandList commands;
    overlay.buildCommands(plan, *scheme, commands);

    ASSERT_EQ(1u, commands.size());
    const auto& color = commands[0].uniforms.at("u_color");
    ASSERT_EQ(4u, color.size());
    EXPECT_FLOAT_EQ(0.15f, color[0]);
    EXPECT_FLOAT_EQ(0.95f, color[1]);
    EXPECT_FLOAT_EQ(0.35f, color[2]);
    EXPECT_FLOAT_EQ(0.82f, color[3]);
}
