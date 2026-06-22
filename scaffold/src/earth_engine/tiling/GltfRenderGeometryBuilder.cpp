#include "GltfRenderGeometryBuilder.h"

#include <algorithm>
#include <cmath>

#include <glm/gtc/matrix_inverse.hpp>

namespace earth_engine {

Vec3 GltfRenderGeometryBuilder::transformPoint(
    const glm::dmat4& transform,
    const Vec3& point) {
    const glm::dvec4 v = transform * glm::dvec4(point.raw(), 1.0);
    return Vec3(glm::dvec3(v) / v.w);
}

Vec3 GltfRenderGeometryBuilder::primitiveCentroid(
    const GltfPrimitive& primitive) {
    Vec3 centroid = Vec3::zero();
    for (const SurfaceVertex& vertex : primitive.vertices) {
        centroid += vertex.positionEcef;
    }
    return primitive.vertices.empty()
        ? Vec3::zero()
        : centroid / static_cast<double>(primitive.vertices.size());
}

Vec3 GltfRenderGeometryBuilder::primitiveSortCenterEcef(
    const GltfPrimitive& primitive,
    const Mat4& contentTransform) {
    if (primitive.vertices.empty()) {
        return Vec3::zero();
    }
    const Vec3 centroid = primitiveCentroid(primitive);
    if (primitive.instances.empty()) {
        return transformPoint(contentTransform.raw(), centroid);
    }

    Vec3 center = Vec3::zero();
    for (const GltfInstance& instance : primitive.instances) {
        const glm::dmat4 worldTransform =
            contentTransform.raw() * instance.transform.raw();
        center += transformPoint(worldTransform, centroid);
    }
    return center / static_cast<double>(primitive.instances.size());
}

bool GltfRenderGeometryBuilder::primitiveUsesSplitBlendInstances(
    const GltfPrimitive& primitive) {
    return !primitive.instances.empty() &&
           (primitive.alphaMode == GltfAlphaMode::Blend ||
            primitive.transmissionFactor > 0.0f);
}

size_t GltfRenderGeometryBuilder::primitiveRenderResourceCount(
    const GltfPrimitive& primitive) {
    if (primitive.vertices.empty() || primitive.indices.empty()) {
        return 0u;
    }
    return primitiveUsesSplitBlendInstances(primitive)
        ? primitive.instances.size()
        : 1u;
}

bool GltfRenderGeometryBuilder::modelUsesSplitBlendInstances(
    const GltfModel& model) {
    return std::any_of(
        model.primitives.begin(),
        model.primitives.end(),
        primitiveUsesSplitBlendInstances);
}

Vec3 GltfRenderGeometryBuilder::localOrigin(
    const GltfModel& model,
    const Mat4& contentTransform) {
    if (model.preferredLocalOriginEcef.has_value()) {
        return contentTransform * *model.preferredLocalOriginEcef;
    }
    Vec3 origin = Vec3::zero();
    size_t vertexTotal = 0;
    for (const GltfPrimitive& primitive : model.primitives) {
        if (primitive.instances.empty()) {
            for (const SurfaceVertex& vertex : primitive.vertices) {
                origin += contentTransform * vertex.positionEcef;
                ++vertexTotal;
            }
        } else if (!primitive.vertices.empty()) {
            const Vec3 centroid = primitiveCentroid(primitive);
            for (const GltfInstance& instance : primitive.instances) {
                const glm::dmat4 worldTransform =
                    contentTransform.raw() * instance.transform.raw();
                origin += transformPoint(worldTransform, centroid) *
                          static_cast<double>(primitive.vertices.size());
                vertexTotal += primitive.vertices.size();
            }
        }
    }
    return vertexTotal > 0
        ? origin / static_cast<double>(vertexTotal)
        : Vec3::zero();
}

bool GltfRenderGeometryBuilder::primitiveHasTexCoordSet(
    const GltfPrimitive& primitive,
    int texCoordSet) {
    if (texCoordSet < 0 ||
        texCoordSet >= static_cast<int>(kGltfMaxTexCoordSets)) {
        return false;
    }
    const size_t set = static_cast<size_t>(texCoordSet);
    if (primitive.vertexTexCoords[set].size() == primitive.vertices.size()) {
        return true;
    }
    return set == 0 && !primitive.vertices.empty();
}

std::array<float, 2> GltfRenderGeometryBuilder::texCoordForVertex(
    const GltfPrimitive& primitive,
    size_t texCoordSet,
    size_t vertexIndex) {
    if (texCoordSet < kGltfMaxTexCoordSets &&
        primitive.vertexTexCoords[texCoordSet].size() ==
            primitive.vertices.size()) {
        return primitive.vertexTexCoords[texCoordSet][vertexIndex];
    }
    if (texCoordSet == 0 && vertexIndex < primitive.vertices.size()) {
        return primitive.vertices[vertexIndex].uv;
    }
    return {0.0f, 0.0f};
}

namespace {

void packTexCoordPair(
    float out[4],
    const std::array<float, 2>& first,
    const std::array<float, 2>& second) {
    out[0] = first[0];
    out[1] = first[1];
    out[2] = second[0];
    out[3] = second[1];
}

} // namespace

std::vector<GltfGpuVertex> GltfRenderGeometryBuilder::buildVertices(
    const GltfPrimitive& primitive,
    const Mat4& contentTransform,
    const Vec3& localOrigin,
    std::optional<bool> keepInstanceLocalVertices) {
    std::vector<GltfGpuVertex> verts(primitive.vertices.size());
    const bool instanced = keepInstanceLocalVertices.value_or(
        !primitive.instances.empty());
    const bool hasVertexColors =
        primitive.vertexColors.size() == primitive.vertices.size();
    const bool hasTangents =
        primitive.vertexTangents.size() == primitive.vertices.size();
    const glm::dmat3 tangentMatrix(contentTransform.raw());
    const glm::dmat3 normalMatrix = glm::inverseTranspose(
        glm::dmat3(contentTransform.raw()));
    for (size_t i = 0; i < primitive.vertices.size(); ++i) {
        const SurfaceVertex& src = primitive.vertices[i];
        const Vec3 rel = instanced
            ? src.positionEcef
            : (contentTransform * src.positionEcef) - localOrigin;
        verts[i].pos[0] = static_cast<float>(rel.x());
        verts[i].pos[1] = static_cast<float>(rel.y());
        verts[i].pos[2] = static_cast<float>(rel.z());

        Vec3 nrm = instanced
            ? src.normalEcef
            : Vec3(normalMatrix * src.normalEcef.raw());
        if (nrm.lengthSquared() > 0.0) {
            nrm = nrm.normalized();
        } else {
            nrm = Vec3::unitZ();
        }
        verts[i].nrm[0] = static_cast<float>(nrm.x());
        verts[i].nrm[1] = static_cast<float>(nrm.y());
        verts[i].nrm[2] = static_cast<float>(nrm.z());
        packTexCoordPair(
            verts[i].texcoord01,
            texCoordForVertex(primitive, 0, i),
            texCoordForVertex(primitive, 1, i));
        const std::array<float, 4> color = hasVertexColors
            ? primitive.vertexColors[i]
            : std::array<float, 4>{1.0f, 1.0f, 1.0f, 1.0f};
        verts[i].color[0] = color[0];
        verts[i].color[1] = color[1];
        verts[i].color[2] = color[2];
        verts[i].color[3] = color[3];
        const std::array<float, 4> tangent = hasTangents
            ? primitive.vertexTangents[i]
            : std::array<float, 4>{0.0f, 0.0f, 0.0f, 1.0f};
        glm::dvec3 tangentDirection(tangent[0], tangent[1], tangent[2]);
        if (hasTangents && !instanced) {
            tangentDirection = tangentMatrix * tangentDirection;
            const double lenSq = glm::dot(tangentDirection, tangentDirection);
            if (std::isfinite(lenSq) && lenSq > 0.0) {
                tangentDirection = glm::normalize(tangentDirection);
            } else {
                tangentDirection = glm::dvec3(0.0);
            }
        }
        verts[i].tangent[0] = static_cast<float>(tangentDirection.x);
        verts[i].tangent[1] = static_cast<float>(tangentDirection.y);
        verts[i].tangent[2] = static_cast<float>(tangentDirection.z);
        verts[i].tangent[3] = tangent[3];
        packTexCoordPair(
            verts[i].texcoord23,
            texCoordForVertex(primitive, 2, i),
            texCoordForVertex(primitive, 3, i));
        packTexCoordPair(
            verts[i].texcoord45,
            texCoordForVertex(primitive, 4, i),
            texCoordForVertex(primitive, 5, i));
        packTexCoordPair(
            verts[i].texcoord67,
            texCoordForVertex(primitive, 6, i),
            texCoordForVertex(primitive, 7, i));
    }
    return verts;
}

std::vector<GltfGpuInstance> GltfRenderGeometryBuilder::buildInstances(
    const GltfPrimitive& primitive,
    const Mat4& contentTransform,
    const Vec3& localOrigin) {
    std::vector<GltfGpuInstance> instances(primitive.instances.size());
    for (size_t i = 0; i < primitive.instances.size(); ++i) {
        glm::dmat4 model =
            contentTransform.raw() *
            primitive.instances[i].transform.raw();
        model[3].x -= localOrigin.x();
        model[3].y -= localOrigin.y();
        model[3].z -= localOrigin.z();

        glm::dmat3 normal(1.0);
        const glm::dmat3 model3(model);
        const double det = glm::determinant(model3);
        if (std::isfinite(det) && std::abs(det) > 1e-14) {
            normal = glm::inverseTranspose(model3);
        }

        for (int c = 0; c < 4; ++c) {
            for (int r = 0; r < 4; ++r) {
                instances[i].model[c * 4 + r] =
                    static_cast<float>(model[c][r]);
            }
        }
        for (int c = 0; c < 3; ++c) {
            for (int r = 0; r < 3; ++r) {
                instances[i].normal[c * 3 + r] =
                    static_cast<float>(normal[c][r]);
            }
        }
    }
    return instances;
}

} // namespace earth_engine
