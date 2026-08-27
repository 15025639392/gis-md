#pragma once

#include <memory>

#include "RenderCommand.h"

namespace earth_engine {

class Framebuffer;
class RenderDevice;
class Renderer;
class Texture;

// T2:地形深度 prepass —— 把「已渲染地形的深度」变成**可采样纹理**,供符号
// (billboard/标签)在顶点着色器里做整符号可见性判定。
//
// 为什么需要独立 pass:符号与地形同在场景 pass 里画,GL 下采样当前 FBO 仍
// 附着的深度附件是未定义行为。maplibre 同样走独立 depth pass(见
// src/render/terrain.ts 的 `_fbo` + terrain_depth shader)。
//
// 为什么不是「符号后置 pass」:那条需要新的 framebuffer 变体(共享 color
// 纹理 + detach depth),且只在离屏后处理链开启时可用(Metal 的
// supportsOffscreenPostProcess() 为 false)。本方案与 pass 顺序解耦,代价是
// 多跑一遍地形几何 —— depth-only、无片元着色,手机上是带宽而非算力瓶颈。
//
// **半分辨率**:符号遮挡是「这个锚点在山前还是山后」的二元判定,不需要全
// 分辨率;半分辨率省 4× 带宽与光栅化。半像素级的误判只影响紧贴山脊线的
// 符号,而那里本就该淡出。
//
// 顶点着色器按命令布局复用对应主 shader 的顶点段：120B glTF 覆盖永久椭球
// /CPU baked terrain，32B terrain 覆盖 compact/位移 terrain。两者配同一空片元，
// 深度与主 pass 一致，不能跨布局误绑 shader。
class TerrainDepthPrepass {
public:
    // device/shader 任一不可用则返回 false,调用方按「无深度纹理」降级
    // (符号退回原 u_depthPushNdc 行为,零回归)。
    bool initialize(RenderDevice* device, Renderer* renderer);
    void dispose();
    bool ready() const {
        return device_ != nullptr &&
               (gltfShader_ != nullptr || gltfInstancedShader_ != nullptr ||
                terrainShader_ != nullptr || terrainInstancedShader_ != nullptr);
    }

    // 按当前场景尺寸确保 FBO(尺寸变化则重建)。返回 nullptr 表示不可用。
    Framebuffer* ensureFramebuffer(int sceneWidthPixels, int sceneHeightPixels);

    // 从完整场景命令表里挑出稳定地表命令(真实地形 + 永久椭球),按顶点布局
    // 换成 depth-only shader。临时 FillProxy 不参与，避免加载期符号跳变。
    // 返回空表 = 本帧没有地形可画(调用方跳过整个 prepass)。
    RenderCommandList extractTerrainCommands(const RenderCommandList& scene) const;

    // 上一次 ensureFramebuffer 产出的深度纹理;未就绪为 nullptr。
    Texture* depthTexture() const;

    // 半分辨率因子(prepass 像素 = 场景像素 / 此值)。符号侧按 NDC→UV 采样,
    // 与分辨率无关,故此值不进 uniform;仅供诊断/测试。
    static constexpr int kResolutionDivisor = 2;

private:
    RenderDevice* device_ = nullptr;
    ShaderProgram* gltfShader_ = nullptr;              // 120B 非实例化
    ShaderProgram* gltfInstancedShader_ = nullptr;     // 120B 实例化
    ShaderProgram* terrainShader_ = nullptr;           // 32B 非实例化
    ShaderProgram* terrainInstancedShader_ = nullptr;  // 32B 实例化
    std::unique_ptr<Framebuffer> framebuffer_;
    int width_ = 0;
    int height_ = 0;
};

}  // namespace earth_engine
