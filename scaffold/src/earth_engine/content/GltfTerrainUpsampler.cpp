#include "GltfTerrainUpsampler.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <vector>

namespace earth_engine {
namespace {

struct ClipVertex {
    SurfaceVertex vertex;
    std::array<std::array<float, 2>, kGltfMaxTexCoordSets> texCoords{};
    std::array<bool, kGltfMaxTexCoordSets> hasTexCoord{};
};

bool childKeepsEast(const UpsampledQuadtreeNode& childID) {
    return (childID.tileID.x & 1) != 0;
}

bool childKeepsNorth(const UpsampledQuadtreeNode& childID) {
    return (childID.tileID.y & 1) != 0;
}

double component(const ClipVertex& vertex, int axis, int texCoord) {
    return axis == 0 ? vertex.texCoords[texCoord][0]
                     : vertex.texCoords[texCoord][1];
}

ClipVertex interpolate(const ClipVertex& a,
                       const ClipVertex& b,
                       double t) {
    t = std::clamp(t, 0.0, 1.0);
    ClipVertex out;
    out.vertex.positionEcef =
        a.vertex.positionEcef +
        (b.vertex.positionEcef - a.vertex.positionEcef) * t;
    out.vertex.positionHighEcef =
        a.vertex.positionHighEcef +
        (b.vertex.positionHighEcef - a.vertex.positionHighEcef) * t;
    out.vertex.positionLowEcef =
        a.vertex.positionLowEcef +
        (b.vertex.positionLowEcef - a.vertex.positionLowEcef) * t;
    out.vertex.normalEcef =
        a.vertex.normalEcef +
        (b.vertex.normalEcef - a.vertex.normalEcef) * t;
    if (out.vertex.normalEcef.lengthSquared() > 0.0) {
        out.vertex.normalEcef = out.vertex.normalEcef.normalized();
    }
    out.vertex.uv = {
        static_cast<float>(a.vertex.uv[0] +
                           (b.vertex.uv[0] - a.vertex.uv[0]) * t),
        static_cast<float>(a.vertex.uv[1] +
                           (b.vertex.uv[1] - a.vertex.uv[1]) * t)};
    for (size_t i = 0; i < kGltfMaxTexCoordSets; ++i) {
        out.hasTexCoord[i] = a.hasTexCoord[i] && b.hasTexCoord[i];
        if (out.hasTexCoord[i]) {
            out.texCoords[i] = {
                static_cast<float>(
                    a.texCoords[i][0] +
                    (b.texCoords[i][0] - a.texCoords[i][0]) * t),
                static_cast<float>(
                    a.texCoords[i][1] +
                    (b.texCoords[i][1] - a.texCoords[i][1]) * t)};
        }
    }
    return out;
}

std::vector<ClipVertex> clipPolygon(const std::vector<ClipVertex>& input,
                                    int axis,
                                    int texCoord,
                                    bool keepGreater,
                                    double threshold) {
    std::vector<ClipVertex> output;
    if (input.empty()) return output;
    output.reserve(input.size() + 1);

    auto inside = [&](const ClipVertex& vertex) {
        constexpr double kEpsilon = 1e-12;
        const double value = component(vertex, axis, texCoord);
        return keepGreater ? value >= threshold - kEpsilon
                           : value <= threshold + kEpsilon;
    };

    ClipVertex previous = input.back();
    bool previousInside = inside(previous);
    for (const ClipVertex& current : input) {
        const bool currentInside = inside(current);
        if (currentInside != previousInside) {
            const double denom =
                component(current, axis, texCoord) -
                component(previous, axis, texCoord);
            if (std::abs(denom) > 1e-15) {
                const double t =
                    (threshold - component(previous, axis, texCoord)) /
                    denom;
                output.push_back(interpolate(previous, current, t));
            }
        }
        if (currentInside) {
            output.push_back(current);
        }
        previous = current;
        previousInside = currentInside;
    }
    return output;
}

std::vector<ClipVertex> makeTriangle(const GltfPrimitive& primitive,
                                     uint32_t ia,
                                     uint32_t ib,
                                     uint32_t ic) {
    std::vector<ClipVertex> triangle;
    triangle.reserve(3);
    for (uint32_t index : {ia, ib, ic}) {
        ClipVertex vertex;
        vertex.vertex = primitive.vertices[index];
        for (size_t set = 0; set < kGltfMaxTexCoordSets; ++set) {
            if (primitive.vertexTexCoords[set].size() ==
                primitive.vertices.size()) {
                vertex.hasTexCoord[set] = true;
                vertex.texCoords[set] = primitive.vertexTexCoords[set][index];
            }
        }
        triangle.push_back(vertex);
    }
    return triangle;
}

void appendPolygon(GltfPrimitive& output,
                   const std::vector<ClipVertex>& polygon,
                   int textureCoordinateIndex) {
    if (polygon.size() < 3) return;
    double area = 0.0;
    for (size_t i = 0; i < polygon.size(); ++i) {
        const auto& a = polygon[i].texCoords[textureCoordinateIndex];
        const auto& b =
            polygon[(i + 1) % polygon.size()].texCoords[textureCoordinateIndex];
        area += static_cast<double>(a[0]) * static_cast<double>(b[1]) -
                static_cast<double>(b[0]) * static_cast<double>(a[1]);
    }
    if (std::abs(area) <= 1e-12) return;
    const uint32_t base = static_cast<uint32_t>(output.vertices.size());
    for (const ClipVertex& vertex : polygon) {
        output.vertices.push_back(vertex.vertex);
        for (size_t set = 0; set < kGltfMaxTexCoordSets; ++set) {
            if (vertex.hasTexCoord[set]) {
                output.vertexTexCoords[set].push_back(vertex.texCoords[set]);
            }
        }
    }
    for (uint32_t i = 1; i + 1 < polygon.size(); ++i) {
        output.indices.push_back(base);
        output.indices.push_back(base + i);
        output.indices.push_back(base + i + 1);
    }
}

bool upsamplePrimitive(const GltfPrimitive& parent,
                       GltfPrimitive& output,
                       const UpsampledQuadtreeNode& childID,
                       int textureCoordinateIndex,
                       bool hasInvertedVCoordinate) {
    if (parent.primitiveMode != GltfPrimitiveMode::Triangles ||
        textureCoordinateIndex < 0 ||
        textureCoordinateIndex >= static_cast<int>(kGltfMaxTexCoordSets) ||
        parent.vertices.empty() ||
        parent.indices.size() < 3 ||
        parent.vertexTexCoords[textureCoordinateIndex].size() !=
            parent.vertices.size()) {
        return false;
    }

    output = parent;
    output.vertices.clear();
    output.indices.clear();
    for (auto& texCoords : output.vertexTexCoords) {
        texCoords.clear();
    }
    output.vertexColors.clear();
    output.vertexTangents.clear();
    output.featureIds.clear();
    output.featureProperties.clear();
    output.instances.clear();
    output.runtime.baseVertices.clear();
    output.runtime.baseTangents.clear();
    output.runtime.skinning.clear();
    output.runtime.morphTargets.clear();

    const bool keepEast = childKeepsEast(childID);
    const bool keepNorth = childKeepsNorth(childID);
    const bool keepGreaterV =
        hasInvertedVCoordinate ? !keepNorth : keepNorth;

    const uint32_t indexBegin = parent.skirtMetadata
        ? parent.skirtMetadata->noSkirtIndicesBegin
        : 0u;
    const uint32_t indexCount = parent.skirtMetadata
        ? parent.skirtMetadata->noSkirtIndicesCount
        : static_cast<uint32_t>(parent.indices.size());
    if (indexBegin >= parent.indices.size() ||
        indexCount > parent.indices.size() - indexBegin) {
        return false;
    }
    const uint32_t indexEnd = indexBegin + indexCount;

    for (uint32_t i = indexBegin; i + 2 < indexEnd; i += 3) {
        const uint32_t ia = parent.indices[i];
        const uint32_t ib = parent.indices[i + 1];
        const uint32_t ic = parent.indices[i + 2];
        if (ia >= parent.vertices.size() ||
            ib >= parent.vertices.size() ||
            ic >= parent.vertices.size()) {
            continue;
        }

        std::vector<ClipVertex> polygon = makeTriangle(parent, ia, ib, ic);
        polygon = clipPolygon(
            polygon,
            0,
            textureCoordinateIndex,
            keepEast,
            0.5);
        polygon = clipPolygon(
            polygon,
            1,
            textureCoordinateIndex,
            keepGreaterV,
            0.5);
        appendPolygon(output, polygon, textureCoordinateIndex);
    }

    if (output.vertices.empty() || output.indices.empty()) {
        return false;
    }

    output.skirtMetadata = parent.skirtMetadata;
    if (output.skirtMetadata) {
        output.skirtMetadata->noSkirtVerticesBegin = 0;
        output.skirtMetadata->noSkirtVerticesCount =
            static_cast<uint32_t>(output.vertices.size());
        output.skirtMetadata->noSkirtIndicesBegin = 0;
        output.skirtMetadata->noSkirtIndicesCount =
            static_cast<uint32_t>(output.indices.size());
    }
    output.runtime.baseVertices = output.vertices;
    return true;
}

void scaleWaterMask(WaterMask& waterMask, const UpsampledQuadtreeNode& childID) {
    waterMask.scale *= 0.5;
    waterMask.translationX += waterMask.scale * (childKeepsEast(childID) ? 1.0 : 0.0);
    waterMask.translationY += waterMask.scale * (childKeepsNorth(childID) ? 1.0 : 0.0);
}

} // namespace

std::unique_ptr<GltfModel> GltfTerrainUpsampler::upsampleForRasterOverlay(
    const GltfModel& parentModel,
    const UpsampledQuadtreeNode& childID,
    int textureCoordinateIndex,
    bool hasInvertedVCoordinate) {
    auto result = std::make_unique<GltfModel>(parentModel);
    result->primitives.clear();
    result->terrainWaterMask = parentModel.terrainWaterMask;
    scaleWaterMask(result->terrainWaterMask, childID);

    for (const GltfPrimitive& primitive : parentModel.primitives) {
        GltfPrimitive upsampled;
        if (upsamplePrimitive(
                primitive,
                upsampled,
                childID,
                textureCoordinateIndex,
                hasInvertedVCoordinate)) {
            result->primitives.push_back(std::move(upsampled));
        }
    }

    return result->primitives.empty() ? nullptr : std::move(result);
}

} // namespace earth_engine
