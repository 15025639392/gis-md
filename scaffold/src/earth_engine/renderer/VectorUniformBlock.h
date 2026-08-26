#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace earth_engine {

/// FeatureRenderLayer 矢量命令的定长 uniform 载荷。
///
/// 这条路径每帧会生成数百条命令；使用 RenderCommand::uniforms 会为每个
/// name/value 创建 map 节点、string 和 vector，并在帧尾逐条释放。定长块把
/// 构建和拷贝收敛为内联赋值/memcpy。GLES 用下方描述表预解析 location；
/// Metal 继续按各 shader 已有的 buffer index 绑定对应字段。
struct alignas(16) VectorUniformBlock {
    std::array<float, 16> modelViewProjection{
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f};
    std::array<float, 16> modelView{
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f};

    std::array<float, 4> color{0.0f, 0.0f, 0.0f, 0.0f};
    std::array<float, 4> haloColor{0.0f, 0.0f, 0.0f, 0.0f};
    std::array<float, 4> terrainOcclusion{0.0f, 0.0f, 1.0f, 0.0f};
    std::array<float, 4> symbolOcclusion{0.0f, 1.0f, 0.0f, 0.0f};

    std::array<float, 3> lightDir{0.0f, 0.0f, 1.0f};
    float ambient = 0.25f;
    std::array<float, 2> viewport{1.0f, 1.0f};
    float lineWidthPx = 1.0f;
    float halfWidthPerEyeZ = 0.0f;
    float dashPeriodMeters = 0.0f;
    float dashOnFraction = 1.0f;
    float pointSizePx = 1.0f;
    float depthPushNdc = 0.0f;
    float sdfEdge = 0.5f;
    float sdfHaloDelta = 0.0f;
};

static_assert(alignof(VectorUniformBlock) == 16);
static_assert(sizeof(VectorUniformBlock) % 16 == 0,
              "keep fixed-block copies naturally aligned");

struct VectorUniformTableEntry {
    const char* name;
    uint16_t floatOffset;
    uint16_t count;
};

namespace detail {
constexpr uint16_t vectorFloatOffset(size_t byteOffset) {
    return static_cast<uint16_t>(byteOffset / sizeof(float));
}
} // namespace detail

inline const auto& vectorUniformTable() {
#define EE_VECTOR_ENTRY(uniformName, field, componentCount)                \
    VectorUniformTableEntry {                                              \
        uniformName,                                                       \
        detail::vectorFloatOffset(offsetof(VectorUniformBlock, field)),    \
        componentCount                                                     \
    }
    static const std::array<VectorUniformTableEntry, 17> table = {{
        EE_VECTOR_ENTRY("u_modelViewProjection", modelViewProjection, 16),
        EE_VECTOR_ENTRY("u_modelView", modelView, 16),
        EE_VECTOR_ENTRY("u_color", color, 4),
        EE_VECTOR_ENTRY("u_haloColor", haloColor, 4),
        EE_VECTOR_ENTRY("u_terrainOcclusion", terrainOcclusion, 4),
        EE_VECTOR_ENTRY("u_symbolOcclusion", symbolOcclusion, 4),
        EE_VECTOR_ENTRY("u_lightDir", lightDir, 3),
        EE_VECTOR_ENTRY("u_ambient", ambient, 1),
        EE_VECTOR_ENTRY("u_viewport", viewport, 2),
        EE_VECTOR_ENTRY("u_lineWidthPx", lineWidthPx, 1),
        EE_VECTOR_ENTRY("u_halfWidthPerEyeZ", halfWidthPerEyeZ, 1),
        EE_VECTOR_ENTRY("u_dashPeriodMeters", dashPeriodMeters, 1),
        EE_VECTOR_ENTRY("u_dashOnFraction", dashOnFraction, 1),
        EE_VECTOR_ENTRY("u_pointSizePx", pointSizePx, 1),
        EE_VECTOR_ENTRY("u_depthPushNdc", depthPushNdc, 1),
        EE_VECTOR_ENTRY("u_sdfEdge", sdfEdge, 1),
        EE_VECTOR_ENTRY("u_sdfHaloDelta", sdfHaloDelta, 1),
    }};
#undef EE_VECTOR_ENTRY
    return table;
}

} // namespace earth_engine
