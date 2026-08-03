#include "TerrainDepthPrepass.h"

#include <algorithm>

#include "RenderDevice.h"
#include "Renderer.h"

namespace earth_engine {

bool TerrainDepthPrepass::initialize(RenderDevice* device, Renderer* renderer) {
    if (!device || !renderer) {
        return false;
    }
    ShaderProgram* shader = renderer->terrainDepthShader();
    if (!shader) {
        // 后端未提供 depth-only shader(如 Metal 侧尚未接线)→ 整条通路不
        // 可用,符号保持原行为。
        return false;
    }
    device_ = device;
    shader_ = shader;
    instancedShader_ = renderer->terrainDepthInstancedShader();
    return true;
}

void TerrainDepthPrepass::dispose() {
    framebuffer_.reset();
    device_ = nullptr;
    shader_ = nullptr;
    instancedShader_ = nullptr;
    width_ = 0;
    height_ = 0;
}

Framebuffer* TerrainDepthPrepass::ensureFramebuffer(int sceneWidthPixels,
                                                    int sceneHeightPixels) {
    if (!ready() || sceneWidthPixels <= 0 || sceneHeightPixels <= 0) {
        return nullptr;
    }
    const int w = std::max(1, sceneWidthPixels / kResolutionDivisor);
    const int h = std::max(1, sceneHeightPixels / kResolutionDivisor);
    if (framebuffer_ && width_ == w && height_ == h) {
        return framebuffer_.get();
    }
    FramebufferDesc desc;
    desc.width = w;
    desc.height = h;
    // color 附件用不上,但 GLES 后端要求 hasColor(见 RenderDeviceGLES::
    // createFramebuffer 的入参校验)。半分辨率下这块浪费已被压到 1/4。
    desc.hasColor = true;
    desc.hasDepth = true;
    desc.depthSampleable = true;
    desc.hasStencil = false;
    std::unique_ptr<Framebuffer> fbo = device_->createFramebuffer(desc);
    if (!fbo) {
        // 建不出来就整条降级,不缓存失败态尺寸(下次 resize 会重试)。
        framebuffer_.reset();
        width_ = 0;
        height_ = 0;
        return nullptr;
    }
    framebuffer_ = std::move(fbo);
    width_ = w;
    height_ = h;
    return framebuffer_.get();
}

RenderCommandList TerrainDepthPrepass::extractTerrainCommands(
    const RenderCommandList& scene) const {
    RenderCommandList out;
    if (!ready()) {
        return out;
    }
    out.reserve(scene.size());
    for (const RenderCommand& cmd : scene) {
        // 只取**真实地形**:fill 代理是加载期的临时面,它的深度不代表地面,
        // 拿它做遮挡会让符号在瓦片没加载好时随机消失。
        if (cmd.terrainSurfaceSource != TerrainSurfaceCommandSource::RealTerrain) {
            continue;
        }
        const bool instanced =
            cmd.kind == RenderCommandKind::GltfPrimitiveInstanced;
        if (cmd.kind != RenderCommandKind::GltfPrimitive && !instanced) {
            continue;
        }
        ShaderProgram* depthShader = instanced ? instancedShader_ : shader_;
        if (!depthShader) {
            // 实例化 depth shader 缺席时**整帧放弃 prepass**,而不是只画逐
            // draw 那部分 —— 半张深度图比没有更糟:合批覆盖的区域会被判成
            // 「无地形」,那里的符号该遮挡却不遮挡,且随合批资格逐帧跳变。
            return RenderCommandList();
        }
        RenderCommand depthCmd = cmd;
        depthCmd.pass = "depth";
        depthCmd.shader = depthShader;
        depthCmd.blend = false;
        depthCmd.depthTest = true;
        depthCmd.depthWrite = true;
        // ⚠️ 纹理必须原样保留:地形位移在**顶点级**采 u_heightTexture
        // (sampler2DArray),清掉会让 prepass 画出零起伏的椭球面,深度比
        // 真实地形浅 → 山后符号照常显示、山前符号被误遮挡。片元虽为空,
        // 顶点采样照旧。
        out.push_back(std::move(depthCmd));
    }
    return out;
}

Texture* TerrainDepthPrepass::depthTexture() const {
    return framebuffer_ ? framebuffer_->depthTexture() : nullptr;
}

}  // namespace earth_engine
