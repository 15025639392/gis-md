#pragma once

#include "../content/GltfModel.h"
#include "../core/math/Vec3.h"

#include <cstddef>
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

/// Terrain-specific lightweight vertex format (40 bytes).
/// The engine currently supports Geographic and WebMercator raster
/// projections, so two packed texcoord sets preserve the complete terrain
/// overlay contract without paying the 120-byte generic glTF vertex cost.
struct TerrainGpuVertex {
    float pos[3];
    float nrm[3];
    float texcoord01[4];
};

static_assert(
    sizeof(TerrainGpuVertex) == 40,
    "Terrain GPU vertices pack POSITION, NORMAL, and TEXCOORD_0/1");

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
