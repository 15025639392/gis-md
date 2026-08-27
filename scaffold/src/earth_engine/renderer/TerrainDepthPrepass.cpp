#include "TerrainDepthPrepass.h"

#include <algorithm>

#include "RenderDevice.h"
#include "Renderer.h"

namespace earth_engine {

bool TerrainDepthPrepass::initialize(RenderDevice* device, Renderer* renderer) {
    if (!device || !renderer) {
        return false;
    }
    ShaderProgram* gltfShader = renderer->gltfDepthShader();
    ShaderProgram* gltfInstancedShader = renderer->gltfDepthInstancedShader();
    ShaderProgram* terrainShader = renderer->terrainDepthShader();
    ShaderProgram* terrainInstancedShader =
        renderer->terrainDepthInstancedShader();
    if (!gltfShader && !gltfInstancedShader && !terrainShader &&
        !terrainInstancedShader) {
        // 后端未提供 depth-only shader(如 Metal 侧尚未接线)→ 整条通路不
        // 可用,符号保持原行为。
        return false;
    }
    device_ = device;
    gltfShader_ = gltfShader;
    gltfInstancedShader_ = gltfInstancedShader;
    terrainShader_ = terrainShader;
    terrainInstancedShader_ = terrainInstancedShader;
    return true;
}

void TerrainDepthPrepass::dispose() {
    framebuffer_.reset();
    device_ = nullptr;
    gltfShader_ = nullptr;
    gltfInstancedShader_ = nullptr;
    terrainShader_ = nullptr;
    terrainInstancedShader_ = nullptr;
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
        // 真实地形与永久椭球都是稳定地表。fill 代理是加载期临时面，拿它
        // 做遮挡会让符号随瓦片生命周期随机消失。
        if (cmd.terrainSurfaceSource != TerrainSurfaceCommandSource::RealTerrain &&
            cmd.terrainSurfaceSource !=
                TerrainSurfaceCommandSource::EllipsoidFallback) {
            continue;
        }
        const bool instanced =
            cmd.kind == RenderCommandKind::GltfPrimitiveInstanced;
        if (cmd.kind != RenderCommandKind::GltfPrimitive && !instanced) {
            continue;
        }
        ShaderProgram* depthShader = nullptr;
        if (cmd.vertexStride == 32) {
            depthShader =
                instanced ? terrainInstancedShader_ : terrainShader_;
        } else if (cmd.vertexStride == 120) {
            depthShader = instanced ? gltfInstancedShader_ : gltfShader_;
        }
        if (!depthShader) {
            // 任一布局缺席时**整帧放弃 prepass**，而不是只画一部分地表。
            // 半张深度图比没有更糟：缺口会让符号随命令布局/合批资格跳变。
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
