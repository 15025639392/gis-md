#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <array>
#include <cstdint>
#include <optional>
#include "RenderDevice.h"  // for Texture/Buffer/ShaderProgram/Framebuffer forward decls

namespace earth_engine {

static constexpr int kMaxSurfaceImageryOverlays = 4;
static constexpr int kGltfInstanceMatrixStride = 100;

enum class RenderCommandKind {
    Unknown,
    SkyBackground,        // order 0: skybox / starfield
    AtmosphereBackground,  // order 5: atmospheric scattering
    GlobeSurface,          // order 10
    SurfaceTile,           // order 10
    GltfPrimitive,         // order 15
    GltfPrimitiveInstanced, // order 15
    VectorOverlay          // order 30
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
    Buffer* instanceBuffer = nullptr;
    std::vector<Texture*> textures;

    // 绘制参数
    int vertexCount = 0;       // glDrawArrays 的顶点数（indexBuffer 为 null 时使用）
    int indexCount = 0;        // glDrawElements 的索引数
    int indexOffset = 0;
    int vertexStride = 0;      // bytes per vertex (0=auto, 32=surface, 120=glTF)
    int instanceCount = 0;
    int instanceStride = 0;
    enum class PrimitiveType { Triangles, TriangleStrip, Lines, LineStrip, Points } primitive = PrimitiveType::Triangles;
    enum class IndexType { UInt16, UInt32 } indexType = IndexType::UInt16;

    // 渲染状态
    bool depthTest = true;
    bool depthWrite = true;
    bool blend = false;
    bool cullFace = true;
    enum class BlendFactor { SrcAlpha, OneMinusSrcAlpha } blendSrc = BlendFactor::SrcAlpha;
    enum class BlendFactorDst { OneMinusSrcAlpha, One } blendDst = BlendFactorDst::OneMinusSrcAlpha;

    // World-space center used by camera-aware transparent sorting.
    // glTF BLEND and fade commands are only valid once Scene has converted this
    // to a camera-space translucentSortDepth for the current frame.
    bool hasWorldSortCenter = false;
    std::array<double, 3> worldSortCenter{0.0, 0.0, 0.0};
    bool hasTranslucentSortDepth = false;
    double translucentSortDepth = 0.0;

    // Uniform 数据（name → float 数组）
    // 平台后端根据 shader uniform layout 解释
    std::unordered_map<std::string, std::vector<float>> uniforms;

    // Hot path for SurfaceTile commands. Keeping these uniforms in fixed
    // storage avoids per-tile unordered_map/string/vector allocation.
    bool hasSurfaceTileUniforms = false;
    std::array<float, 16> surfaceModelViewProjection{};
    std::array<float, 4> surfaceTileUv{0.0f, 0.0f, 1.0f, 1.0f};
    std::array<float, 4> surfaceClipUv{0.0f, 0.0f, 1.0f, 1.0f};
    std::array<std::array<float, 4>, kMaxSurfaceImageryOverlays> surfaceOverlayTileUvs{};
    std::array<float, kMaxSurfaceImageryOverlays> surfaceOverlayOpacities{};
    std::array<float, 3> surfaceLightDir{};
    std::array<float, 3> surfaceCameraRelativeOrigin{};
    std::array<float, 3> surfaceTileOrigin{};
    std::array<float, 3> surfaceFogColor{0.62f, 0.82f, 0.94f};
    float surfaceFogDensity = 2.4e-5f;
    float surfaceTileOpacity = 1.0f;
    float surfaceTransitionOpacity = 1.0f;
    int surfaceOverlayTextureCount = 0;
    float surfaceClipEnabled = 0.0f;
    float surfaceGeneration = 0.0f;
    float surfaceHasWaterMask = 0.0f;
    int surfaceGeometryZoom = -1;
    int surfaceTextureZoom = -1;
};

/// 渲染命令列表（每帧一帧）
using RenderCommandList = std::vector<RenderCommand>;

struct RenderCommandValidationError {
    size_t commandIndex = 0;
    std::string owner;
    std::string message;
};

/// MVP 3D globe 主链路固定顺序：surface -> vector。
int mvpRenderOrder(RenderCommandKind kind);

/// 对 MVP 主链路的 pass/depth/cull/blend 状态做硬校验。
/// 非 MVP 命令必须标为 Unknown 或新增独立 kind 后再定义规则。
std::optional<RenderCommandValidationError>
validateMvpRenderCommands(const RenderCommandList& commands,
                          uint64_t expectedFrameId = 0);

void sortMvpRenderCommands(RenderCommandList& commands);

} // namespace earth_engine
