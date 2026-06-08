#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>
#include <optional>
#include "RenderDevice.h"  // for Texture/Buffer/ShaderProgram/Framebuffer forward decls

namespace earth_engine {

enum class RenderCommandKind {
    Unknown,
    GlobeSurface,
    SurfaceTile,
    VectorOverlay,
    DebugOverlay
};

/// 单条渲染命令。
/// 由 Layer::buildRenderCommands() 生成，Renderer 收集并提交给 RenderDevice。
struct RenderCommand {
    RenderCommandKind kind = RenderCommandKind::Unknown;
    std::string owner;     // layer id（调试用）
    std::string pass;      // "depth" | "color" | "picking" | "shadow" | "postprocess"
    uint64_t frameId = 0;
    uint64_t generation = 0;

    // GPU 资源引用（裸指针，生命周期由 RenderDevice 管理）
    ShaderProgram* shader = nullptr;
    Buffer* vertexBuffer = nullptr;
    Buffer* indexBuffer = nullptr;
    std::vector<Texture*> textures;

    // 绘制参数
    int vertexCount = 0;       // glDrawArrays 的顶点数（indexBuffer 为 null 时使用）
    int indexCount = 0;        // glDrawElements 的索引数
    int indexOffset = 0;
    int vertexStride = 0;      // 每顶点字节数（0 = 后端自动检测，32=globe, 8=tile, 12=vec3）
    enum class PrimitiveType { Triangles, Lines, LineStrip, Points } primitive = PrimitiveType::Triangles;
    enum class IndexType { UInt16, UInt32 } indexType = IndexType::UInt16;

    // 渲染状态
    bool depthTest = true;
    bool depthWrite = true;
    bool blend = false;
    bool cullFace = true;
    enum class BlendFactor { SrcAlpha, OneMinusSrcAlpha } blendSrc = BlendFactor::SrcAlpha;
    enum class BlendFactorDst { OneMinusSrcAlpha, One } blendDst = BlendFactorDst::OneMinusSrcAlpha;

    // Uniform 数据（name → float 数组）
    // 平台后端根据 shader uniform layout 解释
    std::unordered_map<std::string, std::vector<float>> uniforms;
};

/// 渲染命令列表（每帧一帧）
using RenderCommandList = std::vector<RenderCommand>;

struct RenderCommandValidationError {
    size_t commandIndex = 0;
    std::string owner;
    std::string message;
};

/// MVP 3D globe 主链路固定顺序：surface -> vector -> debug。
int mvpRenderOrder(RenderCommandKind kind);

/// 对 MVP 主链路的 pass/depth/cull/blend 状态做硬校验。
/// 非 MVP 命令必须标为 Unknown 或新增独立 kind 后再定义规则。
std::optional<RenderCommandValidationError>
validateMvpRenderCommands(const RenderCommandList& commands,
                          uint64_t expectedFrameId = 0);

void sortMvpRenderCommands(RenderCommandList& commands);

} // namespace earth_engine
