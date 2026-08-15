#pragma once

#include "../content/GltfModel.h"
#include "../core/math/Vec3.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include <glm/glm.hpp>

namespace earth_engine {

struct GltfGpuVertex {
    float pos[3];
    float nrm[3];
    float texcoord01[4];
    float color[4];
    float tangent[4];
    float texcoord23[4];
    float texcoord45[4];
    float texcoord67[4];
};

static_assert(
    sizeof(GltfGpuVertex) == 120,
    "glTF GPU vertices pack POSITION, NORMAL, eight TEXCOORD sets, COLOR_0 and TANGENT");

/// Terrain-specific compact vertex format (32 bytes).
/// The engine currently supports Geographic and WebMercator raster
/// projections, so two packed texcoord sets preserve the complete terrain
/// overlay contract without paying the 120-byte generic glTF vertex cost.
/// Position stays float32 (bit-exact geometry); normal is snorm16 and the
/// texcoord sets are unorm16. Both backends expose the quantized attributes
/// to the shaders as floats via normalized vertex formats, so the shader
/// sources are unchanged. Texcoords are clamped to [0,1] on encode — every
/// terrain texcoord producer (QM uv, projection rewrite, upsample
/// renormalization) already targets that range and samplers clamp to edge.
struct TerrainGpuVertex {
    float pos[3];            // @0  float32 x3
    int16_t nrm[3];          // @12 snorm16 x3
    int16_t nrmPad;          // @18 keeps texcoord01 4-byte aligned; Metal
                             //     reads normal as short4Normalized (w unused)
    uint16_t texcoord01[4];  // @20 unorm16 x4 (TEXCOORD_0/1)
    // @28 geomorph 高度渐变量:= 粗起点高度 − 真实高度(沿椭球法线,米)。顶点
    // shader 用 pos += tileUp * heightDelta * (1-morphFactor) 让子瓦片细节从
    // 平滑起点长到真实高度,LOD 切入无 pop/双影。0 = 无 morph(如上采样子瓦片)。
    float heightDelta;       // @28 float32

    static int16_t packSnorm16(float v) {
        const float clamped = v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v);
        return static_cast<int16_t>(std::lround(clamped * 32767.0f));
    }
    static uint16_t packUnorm16(float v) {
        const float clamped = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
        return static_cast<uint16_t>(std::lround(clamped * 65535.0f));
    }
    // Decode mirrors the GLES3/Metal normalized-attribute rules.
    float normComponent(int i) const {
        const float v = static_cast<float>(nrm[i]) / 32767.0f;
        return v < -1.0f ? -1.0f : v;
    }
    float texcoordComponent(int i) const {
        return static_cast<float>(texcoord01[i]) / 65535.0f;
    }
};

static_assert(
    sizeof(TerrainGpuVertex) == 32,
    "Terrain GPU vertices pack POSITION(f32), NORMAL(snorm16), "
    "TEXCOORD_0/1(unorm16) and geomorph heightDelta(f32)");

struct GltfGpuInstance {
    float model[16];
    float normal[9];
};

struct GltfRenderGeometryBuilder {
    static Vec3 transformPoint(const glm::dmat4& transform, const Vec3& point);
    static Vec3 primitiveCentroid(const GltfPrimitive& primitive);
    static Vec3 primitiveSortCenterEcef(
        const GltfPrimitive& primitive,
        const Mat4& contentTransform);
    static size_t primitiveRenderResourceCount(const GltfPrimitive& primitive);
    static Vec3 localOrigin(
        const GltfModel& model,
        const Mat4& contentTransform);
    static bool primitiveHasTexCoordSet(
        const GltfPrimitive& primitive,
        int texCoordSet);
    static std::array<float, 2> texCoordForVertex(
        const GltfPrimitive& primitive,
        size_t texCoordSet,
        size_t vertexIndex);
    static std::vector<GltfGpuVertex> buildVertices(
        const GltfPrimitive& primitive,
        const Mat4& contentTransform,
        const Vec3& localOrigin,
        std::optional<bool> keepInstanceLocalVertices = std::nullopt);

    /// Build terrain-specific lightweight vertices (40 bytes per vertex).
    /// Includes both supported raster projection texcoord sets while skipping
    /// generic material data and texcoord sets 2-7.
    static std::vector<TerrainGpuVertex> buildTerrainVertices(
        const GltfPrimitive& primitive,
        const Mat4& contentTransform,
        const Vec3& localOrigin);

    static std::vector<GltfGpuInstance> buildInstances(
        const GltfPrimitive& primitive,
        const Mat4& contentTransform,
        const Vec3& localOrigin);
};

} // namespace earth_engine
