#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include "GltfUniformBlock.h"
#include "RenderDevice.h"  // for Texture/Buffer/ShaderProgram/Framebuffer forward decls

namespace earth_engine {

static constexpr int kMaxSurfaceImageryOverlays = 4;
static constexpr int kGltfRasterOverlayTextureBase = 15;
static constexpr int kMaxGltfRasterOverlays = 4;
static constexpr int kGltfWaterMaskTextureSlot =
    kGltfRasterOverlayTextureBase + kMaxGltfRasterOverlays;
static constexpr int kGltfInstanceMatrixStride = 100;

enum class RenderCommandKind {
    Unknown,
    SkyBackground,        // order 0: skybox / starfield
    AtmosphereBackground,  // order 5: atmospheric scattering
    SurfaceTile,           // order 10
    GltfPrimitive,         // order 15
    GltfPrimitiveInstanced, // order 15
    VectorOverlay          // order 30
};

enum class TerrainSurfaceCommandSource {
    Unknown,
    RealTerrain,
    FillProxy,
    EllipsoidFallback
};

/// 单条渲染命令。
/// 由 Layer::buildRenderCommands() 生成，Renderer 收集并提交给 RenderDevice。
struct RenderCommand {
    RenderCommandKind kind = RenderCommandKind::Unknown;
    std::string owner;     // layer id（调试用）
    std::string pass;      // "depth" | "color" | "picking" | "shadow" | "postprocess"
    // Stable identity for long-lived renderables. Transient commands leave this
    // empty and are rebuilt directly into the frame list.
    std::string stableKey;
    uint64_t frameId = 0;
    uint64_t generation = 0;
    bool terrainRenderContent = false;
    TerrainSurfaceCommandSource terrainSurfaceSource =
        TerrainSurfaceCommandSource::Unknown;

    // GPU 资源引用（裸指针，生命周期由 RenderDevice 管理）
    ShaderProgram* shader = nullptr;
    Buffer* vertexBuffer = nullptr;
    Buffer* indexBuffer = nullptr;
    Buffer* instanceBuffer = nullptr;
    std::vector<Texture*> textures;
    // Optional short-lived resource owners for raw pointers above. Commands can
    // keep raster/content resources alive through submit without taking over
    // renderer ownership.
    std::vector<std::shared_ptr<const void>> resourceKeepAlive;

    // 绘制参数
    int vertexCount = 0;       // glDrawArrays 的顶点数（indexBuffer 为 null 时使用）
    int indexCount = 0;        // glDrawElements 的索引数
    int indexOffset = 0;       // 起始索引偏移，单位为“索引个数”（非字节）；各后端按 indexType 大小自行换算字节偏移
    int vertexStride = 0;      // bytes per vertex (0=auto, 32=surface, 40=terrain, 120=glTF)
    int instanceCount = 0;
    int instanceStride = 0;
    enum class PrimitiveType { Triangles, TriangleStrip, Lines, LineStrip, Points } primitive = PrimitiveType::Triangles;
    enum class IndexType { UInt16, UInt32 } indexType = IndexType::UInt16;

    // 渲染状态
    bool depthTest = true;
    bool depthWrite = true;
    bool blend = false;
    // 实例化 blend/透射 primitive 走 alpha-to-coverage(MSAA 覆盖抖动)而非逐实例
    // alpha 排序:顺序无关、单 draw 画完所有实例,避免 blend 实例化退化成每实例
    // 一个 draw call(见 GltfDrawCommandBuilder + RenderDeviceGLES)。true 时后端
    // 启用 GL_SAMPLE_ALPHA_TO_COVERAGE 且不开常规 alpha 混合。
    bool alphaToCoverage = false;
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

    // Uniform 数据（name → float 数组）。
    // 仅供冷路径命令使用（SkyBackground / AtmosphereBackground /
    // VectorOverlay 等，每帧个位数条目）。glTF/terrain 热路径命令必须走
    // 下方 gltfUniforms 定长块并保持本 map 为空——空 map 拷贝零分配。
    std::unordered_map<std::string, std::vector<float>> uniforms;

    // glTF / terrain 命令的定长 uniform 块（热路径，见 GltfUniformBlock.h）。
    // hasGltfUniforms=true 时后端整块消费：Metal 一次 setBytes 绑
    // fragment buffer(0)，GLES 经 program 级 location 表直传。
    bool hasGltfUniforms = false;
    GltfUniformBlock gltfUniforms;

    // Render-chain step 10: SurfaceTile command organization lives here.
    // These fields describe draw order inputs, depth/cull/blend state, base
    // texture, overlay count, and UV windows before any GLES/Metal API call.
    // Unit tests at this layer prove command intent, not final framebuffer
    // pixels.
    //
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
    std::array<float, 4> surfaceWaterMaskTranslationScale{
        0.0f,
        0.0f,
        1.0f,
        0.0f};
    std::array<float, 4> surfaceWaterMaskState{1.0f, 0.0f, 0.0f, 0.0f};
    int surfaceGeometryZoom = -1;
    int surfaceTextureZoom = -1;
    int surfaceMeshIndexCount = 0;
    int surfaceNoSkirtIndexCount = 0;
    int surfaceSkirtIndexCount = 0;
    int surfaceBaseRasterState = 0;
    int surfaceBaseIsMappedRasterTile = 0;

    // glTF raster overlays use _CESIUMOVERLAY_n attributes and separate
    // material texture slots, so they cannot reuse surface overlay samplers.
    std::array<std::array<float, 4>, kMaxGltfRasterOverlays>
        gltfRasterOverlayTileUvs{};
    std::array<float, kMaxGltfRasterOverlays> gltfRasterOverlayOpacities{};
    std::array<float, kMaxGltfRasterOverlays> gltfRasterOverlayTexCoordSets{};
    int gltfRasterOverlayTextureCount = 0;
    float gltfHasWaterMask = 0.0f;
    std::array<float, 4> gltfWaterMaskTranslationScale{
        0.0f,
        0.0f,
        1.0f,
        0.0f};
    std::array<float, 4> gltfWaterMaskState{1.0f, 0.0f, 0.0f, 0.0f};
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
