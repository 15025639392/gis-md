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
    static bool primitiveUsesSplitBlendInstances(
        const GltfPrimitive& primitive);
    static size_t primitiveRenderResourceCount(const GltfPrimitive& primitive);
    static bool modelUsesSplitBlendInstances(const GltfModel& model);
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
    static std::vector<GltfGpuInstance> buildInstances(
        const GltfPrimitive& primitive,
        const Mat4& contentTransform,
        const Vec3& localOrigin);
};

} // namespace earth_engine
