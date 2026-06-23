#include "GltfTerrainUpsampler.h"

#include "../core/geodesy/Ellipsoid.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <vector>

namespace earth_engine {
namespace {

struct ClipVertex {
    SurfaceVertex vertex;
    int32_t sourceIndex = -1;
    std::array<std::array<float, 2>, kGltfMaxTexCoordSets> texCoords{};
    std::array<bool, kGltfMaxTexCoordSets> hasTexCoord{};
    std::array<float, 4> color{};
    bool hasColor = false;
    std::array<float, 4> tangent{};
    bool hasTangent = false;
    uint32_t featureId = 0;
    bool hasFeatureId = false;
    std::map<std::string, GltfFeaturePropertyValue> featureProperties;
    bool hasFeatureProperties = false;
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

std::pair<Vec3, Vec3> splitHighLow(const Vec3& value) {
    constexpr double kSplit = 65536.0;
    const auto split = [](double v) {
        const double high = std::floor(v / kSplit) * kSplit;
        return std::pair<double, double>{high, v - high};
    };
    const auto sx = split(value.x());
    const auto sy = split(value.y());
    const auto sz = split(value.z());
    return {
        Vec3(sx.first, sy.first, sz.first),
        Vec3(sx.second, sy.second, sz.second)};
}

void setPosition(SurfaceVertex& vertex, const Vec3& positionEcef) {
    vertex.positionEcef = positionEcef;
    auto split = splitHighLow(positionEcef);
    vertex.positionHighEcef = split.first;
    vertex.positionLowEcef = split.second;
}

ClipVertex interpolate(const ClipVertex& a,
                       const ClipVertex& b,
                       double t) {
    t = std::clamp(t, 0.0, 1.0);
    ClipVertex out;
    out.sourceIndex = -1;
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
    out.hasColor = a.hasColor && b.hasColor;
    if (out.hasColor) {
        for (size_t i = 0; i < out.color.size(); ++i) {
            out.color[i] = static_cast<float>(
                a.color[i] + (b.color[i] - a.color[i]) * t);
        }
    }
    out.hasTangent = a.hasTangent && b.hasTangent;
    if (out.hasTangent) {
        for (size_t i = 0; i < out.tangent.size(); ++i) {
            out.tangent[i] = static_cast<float>(
                a.tangent[i] + (b.tangent[i] - a.tangent[i]) * t);
        }
    }
    out.hasFeatureId = a.hasFeatureId && b.hasFeatureId;
    if (out.hasFeatureId) {
        if (a.featureId == b.featureId) {
            out.featureId = a.featureId;
        } else {
            out.featureId = t < 0.5 ? a.featureId : b.featureId;
        }
    }
    out.hasFeatureProperties =
        a.hasFeatureProperties && b.hasFeatureProperties;
    if (out.hasFeatureProperties) {
        if (a.hasFeatureId && b.hasFeatureId && a.featureId == b.featureId) {
            out.featureProperties = a.featureProperties;
        } else if (t < 0.5) {
            out.featureProperties = a.featureProperties;
        } else {
            out.featureProperties = b.featureProperties;
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
        vertex.sourceIndex = static_cast<int32_t>(index);
        vertex.vertex = primitive.vertices[index];
        for (size_t set = 0; set < kGltfMaxTexCoordSets; ++set) {
            if (primitive.vertexTexCoords[set].size() ==
                primitive.vertices.size()) {
                vertex.hasTexCoord[set] = true;
                vertex.texCoords[set] = primitive.vertexTexCoords[set][index];
            }
        }
        if (primitive.vertexColors.size() == primitive.vertices.size()) {
            vertex.hasColor = true;
            vertex.color = primitive.vertexColors[index];
        }
        if (primitive.vertexTangents.size() == primitive.vertices.size()) {
            vertex.hasTangent = true;
            vertex.tangent = primitive.vertexTangents[index];
        }
        if (primitive.featureIds.size() == primitive.vertices.size()) {
            vertex.hasFeatureId = true;
            vertex.featureId = primitive.featureIds[index];
        }
        if (primitive.featureProperties.size() == primitive.vertices.size()) {
            vertex.hasFeatureProperties = true;
            vertex.featureProperties = primitive.featureProperties[index];
        }
        triangle.push_back(vertex);
    }
    return triangle;
}

void appendPolygon(GltfPrimitive& output,
                   const std::vector<ClipVertex>& polygon,
                   int textureCoordinateIndex,
                   std::vector<uint32_t>& sourceVertexMap) {
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
    std::vector<uint32_t> polygonIndices;
    polygonIndices.reserve(polygon.size());
    for (const ClipVertex& vertex : polygon) {
        uint32_t index = std::numeric_limits<uint32_t>::max();
        if (vertex.sourceIndex >= 0 &&
            static_cast<size_t>(vertex.sourceIndex) <
                sourceVertexMap.size()) {
            index = sourceVertexMap[static_cast<size_t>(vertex.sourceIndex)];
        }
        if (index == std::numeric_limits<uint32_t>::max()) {
            index = static_cast<uint32_t>(output.vertices.size());
            output.vertices.push_back(vertex.vertex);
            for (size_t set = 0; set < kGltfMaxTexCoordSets; ++set) {
                if (vertex.hasTexCoord[set]) {
                    output.vertexTexCoords[set].push_back(
                        vertex.texCoords[set]);
                }
            }
            if (vertex.hasColor) {
                output.vertexColors.push_back(vertex.color);
            }
            if (vertex.hasTangent) {
                output.vertexTangents.push_back(vertex.tangent);
            }
            if (vertex.hasFeatureId) {
                output.featureIds.push_back(vertex.featureId);
            }
            if (vertex.hasFeatureProperties) {
                output.featureProperties.push_back(vertex.featureProperties);
            }
            if (vertex.sourceIndex >= 0 &&
                static_cast<size_t>(vertex.sourceIndex) <
                    sourceVertexMap.size()) {
                sourceVertexMap[static_cast<size_t>(vertex.sourceIndex)] =
                    index;
            }
        }
        polygonIndices.push_back(index);
    }
    for (uint32_t i = 1; i + 1 < polygon.size(); ++i) {
        output.indices.push_back(polygonIndices[0]);
        output.indices.push_back(polygonIndices[i]);
        output.indices.push_back(polygonIndices[i + 1]);
    }
}

void appendPoint(GltfPrimitive& output,
                 const GltfPrimitive& parent,
                 uint32_t index,
                 bool emitIndex) {
    const uint32_t outputIndex = static_cast<uint32_t>(output.vertices.size());
    output.vertices.push_back(parent.vertices[index]);
    for (size_t set = 0; set < kGltfMaxTexCoordSets; ++set) {
        if (parent.vertexTexCoords[set].size() == parent.vertices.size()) {
            output.vertexTexCoords[set].push_back(
                parent.vertexTexCoords[set][index]);
        }
    }
    if (parent.vertexColors.size() == parent.vertices.size()) {
        output.vertexColors.push_back(parent.vertexColors[index]);
    }
    if (parent.vertexTangents.size() == parent.vertices.size()) {
        output.vertexTangents.push_back(parent.vertexTangents[index]);
    }
    if (parent.featureIds.size() == parent.vertices.size()) {
        output.featureIds.push_back(parent.featureIds[index]);
    }
    if (parent.featureProperties.size() == parent.vertices.size()) {
        output.featureProperties.push_back(parent.featureProperties[index]);
    }
    if (emitIndex) {
        output.indices.push_back(outputIndex);
    }
}

struct EdgeVertex {
    uint32_t index = 0;
    std::array<float, 2> uv = {0.0f, 0.0f};
};

struct EdgeIndices {
    std::vector<EdgeVertex> west;
    std::vector<EdgeVertex> south;
    std::vector<EdgeVertex> east;
    std::vector<EdgeVertex> north;
};

bool equalsEpsilon(float a, double b) {
    return std::abs(static_cast<double>(a) - b) <= 1e-5;
}

double shortestSkirtHeight(const SkirtMetadata& skirt) {
    return std::min(
        std::min(skirt.skirtWestHeight, skirt.skirtEastHeight),
        std::min(skirt.skirtSouthHeight, skirt.skirtNorthHeight));
}

void collectEdge(EdgeIndices& edges,
                 const GltfPrimitive& primitive,
                 int textureCoordinateIndex,
                 const UpsampledQuadtreeNode& childID,
                 bool hasInvertedVCoordinate) {
    if (textureCoordinateIndex < 0 ||
        textureCoordinateIndex >= static_cast<int>(kGltfMaxTexCoordSets)) {
        return;
    }
    const auto& texCoords =
        primitive.vertexTexCoords[static_cast<size_t>(textureCoordinateIndex)];
    if (texCoords.size() != primitive.vertices.size()) {
        return;
    }

    const bool eastChild = childKeepsEast(childID);
    const bool northChild = childKeepsNorth(childID);
    const double westU = eastChild ? 0.5 : 0.0;
    const double eastU = eastChild ? 1.0 : 0.5;
    const bool keepGreaterV =
        hasInvertedVCoordinate ? !northChild : northChild;
    const double lowV = keepGreaterV ? 0.5 : 0.0;
    const double highV = keepGreaterV ? 1.0 : 0.5;
    const double southV = hasInvertedVCoordinate ? highV : lowV;
    const double northV = hasInvertedVCoordinate ? lowV : highV;

    for (uint32_t i = 0; i < texCoords.size(); ++i) {
        const auto& uv = texCoords[i];
        if (equalsEpsilon(uv[0], westU)) {
            edges.west.push_back({i, uv});
        }
        if (equalsEpsilon(uv[0], eastU)) {
            edges.east.push_back({i, uv});
        }
        if (equalsEpsilon(uv[1], southV)) {
            edges.south.push_back({i, uv});
        }
        if (equalsEpsilon(uv[1], northV)) {
            edges.north.push_back({i, uv});
        }
    }
}

void uniqueSortedEdge(std::vector<EdgeVertex>& edge) {
    edge.erase(
        std::unique(
            edge.begin(),
            edge.end(),
            [](const EdgeVertex& a, const EdgeVertex& b) {
                return a.index == b.index;
            }),
        edge.end());
}

void appendSkirtVertex(GltfPrimitive& primitive,
                       uint32_t sourceIndex,
                       double skirtHeight,
                       const Vec3& meshCenter,
                       const Ellipsoid& ellipsoid) {
    SurfaceVertex skirtVertex = primitive.vertices[sourceIndex];
    const Vec3 absoluteTop = skirtVertex.positionEcef + meshCenter;
    Vec3 normal = ellipsoid.geodeticSurfaceNormal(absoluteTop);
    if (normal.lengthSquared() <= 0.0 &&
        skirtVertex.normalEcef.lengthSquared() > 0.0) {
        normal = skirtVertex.normalEcef.normalized();
    }
    if (normal.lengthSquared() > 0.0) {
        normal = normal.normalized();
    }
    const Vec3 skirtPosition =
        absoluteTop - normal * skirtHeight - meshCenter;
    setPosition(skirtVertex, skirtPosition);
    primitive.vertices.push_back(skirtVertex);

    for (size_t set = 0; set < kGltfMaxTexCoordSets; ++set) {
        if (primitive.vertexTexCoords[set].size() + 1 ==
            primitive.vertices.size()) {
            primitive.vertexTexCoords[set].push_back(
                primitive.vertexTexCoords[set][sourceIndex]);
        }
    }
    if (primitive.vertexColors.size() + 1 == primitive.vertices.size()) {
        primitive.vertexColors.push_back(primitive.vertexColors[sourceIndex]);
    }
    if (primitive.vertexTangents.size() + 1 == primitive.vertices.size()) {
        primitive.vertexTangents.push_back(
            primitive.vertexTangents[sourceIndex]);
    }
    if (primitive.featureIds.size() + 1 == primitive.vertices.size()) {
        primitive.featureIds.push_back(primitive.featureIds[sourceIndex]);
    }
    if (primitive.featureProperties.size() + 1 == primitive.vertices.size()) {
        primitive.featureProperties.push_back(
            primitive.featureProperties[sourceIndex]);
    }
}

void appendSkirtEdge(GltfPrimitive& primitive,
                     const std::vector<EdgeVertex>& edge,
                     double skirtHeight,
                     const Vec3& meshCenter,
                     const Ellipsoid& ellipsoid) {
    if (edge.size() < 2) {
        return;
    }

    const uint32_t firstSkirtVertex =
        static_cast<uint32_t>(primitive.vertices.size());
    for (const EdgeVertex& edgeVertex : edge) {
        appendSkirtVertex(
            primitive,
            edgeVertex.index,
            skirtHeight,
            meshCenter,
            ellipsoid);
    }

    for (uint32_t i = 0; i + 1 < edge.size(); ++i) {
        const uint32_t topA = edge[i].index;
        const uint32_t topB = edge[i + 1].index;
        const uint32_t skirtA = firstSkirtVertex + i;
        const uint32_t skirtB = firstSkirtVertex + i + 1;
        primitive.indices.push_back(topA);
        primitive.indices.push_back(topB);
        primitive.indices.push_back(skirtA);
        primitive.indices.push_back(skirtA);
        primitive.indices.push_back(topB);
        primitive.indices.push_back(skirtB);
    }
}

void addSkirts(GltfPrimitive& primitive,
               const UpsampledQuadtreeNode& childID,
               int textureCoordinateIndex,
               bool hasInvertedVCoordinate) {
    if (!primitive.skirtMetadata) {
        return;
    }

    const SkirtMetadata parentSkirt = *primitive.skirtMetadata;
    primitive.skirtMetadata->noSkirtVerticesBegin = 0;
    primitive.skirtMetadata->noSkirtVerticesCount =
        static_cast<uint32_t>(primitive.vertices.size());
    primitive.skirtMetadata->noSkirtIndicesBegin = 0;
    primitive.skirtMetadata->noSkirtIndicesCount =
        static_cast<uint32_t>(primitive.indices.size());

    EdgeIndices edges;
    collectEdge(
        edges,
        primitive,
        textureCoordinateIndex,
        childID,
        hasInvertedVCoordinate);

    const double shortestHeight = shortestSkirtHeight(parentSkirt);
    primitive.skirtMetadata->skirtWestHeight = childKeepsEast(childID)
        ? shortestHeight * 0.5
        : parentSkirt.skirtWestHeight;
    primitive.skirtMetadata->skirtSouthHeight = childKeepsNorth(childID)
        ? shortestHeight * 0.5
        : parentSkirt.skirtSouthHeight;
    primitive.skirtMetadata->skirtEastHeight = childKeepsEast(childID)
        ? parentSkirt.skirtEastHeight
        : shortestHeight * 0.5;
    primitive.skirtMetadata->skirtNorthHeight = childKeepsNorth(childID)
        ? parentSkirt.skirtNorthHeight
        : shortestHeight * 0.5;

    std::sort(edges.west.begin(), edges.west.end(), [](const auto& a,
                                                       const auto& b) {
        return a.uv[1] < b.uv[1] ||
               (a.uv[1] == b.uv[1] && a.index < b.index);
    });
    uniqueSortedEdge(edges.west);
    std::sort(edges.south.begin(), edges.south.end(), [](const auto& a,
                                                        const auto& b) {
        return a.uv[0] > b.uv[0] ||
               (a.uv[0] == b.uv[0] && a.index < b.index);
    });
    if (hasInvertedVCoordinate) {
        std::reverse(edges.south.begin(), edges.south.end());
    }
    uniqueSortedEdge(edges.south);
    std::sort(edges.east.begin(), edges.east.end(), [](const auto& a,
                                                       const auto& b) {
        return a.uv[1] > b.uv[1] ||
               (a.uv[1] == b.uv[1] && a.index < b.index);
    });
    uniqueSortedEdge(edges.east);
    std::sort(edges.north.begin(), edges.north.end(), [](const auto& a,
                                                        const auto& b) {
        return a.uv[0] < b.uv[0] ||
               (a.uv[0] == b.uv[0] && a.index < b.index);
    });
    if (hasInvertedVCoordinate) {
        std::reverse(edges.north.begin(), edges.north.end());
    }
    uniqueSortedEdge(edges.north);

    const Ellipsoid ellipsoid = Ellipsoid::WGS84();
    appendSkirtEdge(
        primitive,
        edges.west,
        primitive.skirtMetadata->skirtWestHeight,
        primitive.skirtMetadata->meshCenter,
        ellipsoid);
    appendSkirtEdge(
        primitive,
        edges.south,
        primitive.skirtMetadata->skirtSouthHeight,
        primitive.skirtMetadata->meshCenter,
        ellipsoid);
    appendSkirtEdge(
        primitive,
        edges.east,
        primitive.skirtMetadata->skirtEastHeight,
        primitive.skirtMetadata->meshCenter,
        ellipsoid);
    appendSkirtEdge(
        primitive,
        edges.north,
        primitive.skirtMetadata->skirtNorthHeight,
        primitive.skirtMetadata->meshCenter,
        ellipsoid);
}

bool upsamplePointsPrimitive(const GltfPrimitive& parent,
                             GltfPrimitive& output,
                             const UpsampledQuadtreeNode& childID,
                             int textureCoordinateIndex,
                             bool hasInvertedVCoordinate) {
    if (textureCoordinateIndex < 0 ||
        textureCoordinateIndex >= static_cast<int>(kGltfMaxTexCoordSets) ||
        parent.vertices.empty() ||
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
    const bool hasExplicitIndices = !parent.indices.empty();
    const uint32_t sourceIndexCount = hasExplicitIndices
        ? static_cast<uint32_t>(parent.indices.size())
        : static_cast<uint32_t>(parent.vertices.size());

    for (uint32_t i = 0; i < sourceIndexCount; ++i) {
        const uint32_t index = hasExplicitIndices ? parent.indices[i] : i;
        if (index >= parent.vertices.size()) {
            continue;
        }
        const auto& uv = parent.vertexTexCoords[textureCoordinateIndex][index];
        const bool insideU = keepEast ? uv[0] > 0.5f : uv[0] < 0.5f;
        const bool insideV = keepGreaterV ? uv[1] > 0.5f : uv[1] < 0.5f;
        if (insideU && insideV) {
            appendPoint(output, parent, index, hasExplicitIndices);
        }
    }

    if (output.vertices.empty() ||
        (hasExplicitIndices && output.indices.empty())) {
        return false;
    }
    return true;
}

bool upsamplePrimitive(const GltfPrimitive& parent,
                       GltfPrimitive& output,
                       const UpsampledQuadtreeNode& childID,
                       int textureCoordinateIndex,
                       bool hasInvertedVCoordinate) {
    if (parent.primitiveMode == GltfPrimitiveMode::Points) {
        return upsamplePointsPrimitive(
            parent,
            output,
            childID,
            textureCoordinateIndex,
            hasInvertedVCoordinate);
    }

    const bool isTrianglePrimitive =
        parent.primitiveMode == GltfPrimitiveMode::Triangles ||
        parent.primitiveMode == GltfPrimitiveMode::TriangleStrip ||
        parent.primitiveMode == GltfPrimitiveMode::TriangleFan;
    if (!isTrianglePrimitive ||
        textureCoordinateIndex < 0 ||
        textureCoordinateIndex >= static_cast<int>(kGltfMaxTexCoordSets) ||
        parent.vertices.empty() ||
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
    output.primitiveMode = GltfPrimitiveMode::Triangles;

    const bool keepEast = childKeepsEast(childID);
    const bool keepNorth = childKeepsNorth(childID);
    const bool keepGreaterV =
        hasInvertedVCoordinate ? !keepNorth : keepNorth;

    const bool hasExplicitIndices = !parent.indices.empty();
    const uint32_t implicitIndexCount =
        static_cast<uint32_t>(parent.vertices.size());
    const uint32_t sourceIndexCount = hasExplicitIndices
        ? static_cast<uint32_t>(parent.indices.size())
        : implicitIndexCount;
    const uint32_t indexBegin = parent.skirtMetadata
        ? (hasExplicitIndices ? parent.skirtMetadata->noSkirtIndicesBegin
                              : parent.skirtMetadata->noSkirtVerticesBegin)
        : 0u;
    const uint32_t indexCount = parent.skirtMetadata
        ? (hasExplicitIndices ? parent.skirtMetadata->noSkirtIndicesCount
                              : parent.skirtMetadata->noSkirtVerticesCount)
        : sourceIndexCount;
    if (sourceIndexCount < 3 ||
        indexBegin >= sourceIndexCount ||
        indexCount > sourceIndexCount - indexBegin) {
        return false;
    }
    const uint32_t indexEnd = indexBegin + indexCount;
    std::vector<uint32_t> sourceVertexMap(
        parent.vertices.size(),
        std::numeric_limits<uint32_t>::max());

    auto sourceIndex = [&](uint32_t i) {
        return hasExplicitIndices ? parent.indices[i] : i;
    };
    auto upsampleTriangle = [&](uint32_t ia, uint32_t ib, uint32_t ic) {
        if (ia >= parent.vertices.size() ||
            ib >= parent.vertices.size() ||
            ic >= parent.vertices.size()) {
            return;
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
        appendPolygon(
            output,
            polygon,
            textureCoordinateIndex,
            sourceVertexMap);
    };

    if (parent.primitiveMode == GltfPrimitiveMode::Triangles) {
        for (uint32_t i = indexBegin; i + 2 < indexEnd; i += 3) {
            upsampleTriangle(
                sourceIndex(i),
                sourceIndex(i + 1),
                sourceIndex(i + 2));
        }
    } else if (parent.primitiveMode == GltfPrimitiveMode::TriangleStrip) {
        for (uint32_t i = indexBegin; i + 2 < indexEnd; ++i) {
            if (((i - indexBegin) % 2u) == 0u) {
                upsampleTriangle(
                    sourceIndex(i),
                    sourceIndex(i + 1),
                    sourceIndex(i + 2));
            } else {
                upsampleTriangle(
                    sourceIndex(i),
                    sourceIndex(i + 2),
                    sourceIndex(i + 1));
            }
        }
    } else {
        const uint32_t fanRoot = sourceIndex(indexBegin);
        for (uint32_t i = indexBegin + 1; i + 1 < indexEnd; ++i) {
            upsampleTriangle(
                fanRoot,
                sourceIndex(i),
                sourceIndex(i + 1));
        }
    }

    if (output.vertices.empty() || output.indices.empty()) {
        return false;
    }

    output.skirtMetadata = parent.skirtMetadata;
    addSkirts(output, childID, textureCoordinateIndex, hasInvertedVCoordinate);
    return true;
}

void scaleWaterMask(WaterMask& waterMask, const UpsampledQuadtreeNode& childID) {
    waterMask.scale *= 0.5;
    waterMask.translationX += waterMask.scale * (childKeepsEast(childID) ? 1.0 : 0.0);
    waterMask.translationY += waterMask.scale * (childKeepsNorth(childID) ? 1.0 : 0.0);
}

void scalePrimitiveWaterMask(GltfPrimitive& primitive,
                             const GltfPrimitive& parent,
                             const UpsampledQuadtreeNode& childID) {
    primitive.hasTerrainWaterMaskMetadata = true;
    primitive.terrainOnlyWater = parent.terrainOnlyWater;
    primitive.terrainOnlyLand = parent.terrainOnlyLand;
    primitive.terrainWaterMaskTextureIndex =
        (parent.hasTerrainWaterMaskMetadata &&
         !parent.terrainOnlyWater &&
         !parent.terrainOnlyLand)
            ? parent.terrainWaterMaskTextureIndex
            : std::optional<size_t>{};
    const double parentScale = parent.hasTerrainWaterMaskMetadata
        ? parent.terrainWaterMaskScale
        : 0.0;
    primitive.terrainWaterMaskScale = 0.5 * parentScale;
    primitive.terrainWaterMaskTranslationX =
        parent.terrainWaterMaskTranslationX +
        primitive.terrainWaterMaskScale *
            (childKeepsEast(childID) ? 1.0 : 0.0);
    primitive.terrainWaterMaskTranslationY =
        parent.terrainWaterMaskTranslationY +
        primitive.terrainWaterMaskScale *
            (childKeepsNorth(childID) ? 1.0 : 0.0);
}

void rebuildRuntimeBaseVerticesForNode(GltfPrimitive& primitive,
                                       const GltfModel& model) {
    primitive.runtime.baseVertices = primitive.vertices;
    const int nodeIndex = primitive.runtime.nodeIndex;
    if (nodeIndex < 0 ||
        static_cast<size_t>(nodeIndex) >= model.nodes.size()) {
        return;
    }

    const Mat4 inverseNodeTransform =
        model.nodes[static_cast<size_t>(nodeIndex)].globalTransform.inverse();
    for (SurfaceVertex& vertex : primitive.runtime.baseVertices) {
        vertex.positionEcef =
            inverseNodeTransform.transformPoint(vertex.positionEcef);
        if (vertex.normalEcef.lengthSquared() > 0.0) {
            vertex.normalEcef =
                inverseNodeTransform.transformVector(vertex.normalEcef)
                    .normalized();
        }
    }
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
            scalePrimitiveWaterMask(upsampled, primitive, childID);
            rebuildRuntimeBaseVerticesForNode(upsampled, *result);
            result->primitives.push_back(std::move(upsampled));
        }
    }

    return result->primitives.empty() ? nullptr : std::move(result);
}

} // namespace earth_engine
