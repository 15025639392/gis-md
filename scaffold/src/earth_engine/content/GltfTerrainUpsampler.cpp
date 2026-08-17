#include "GltfTerrainUpsampler.h"

#include "../core/geodesy/Cartographic.h"
#include "../core/geodesy/Ellipsoid.h"
#include "../debug/Contracts.h"

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

// Which half of the parent the child keeps, derived from the clip-set window
// itself (the interior boundary is the one away from the parent edge) instead
// of tile-key parity, so schemes whose y axis grows southward stay correct.
struct ClipSides {
    // Keep the greater-U / greater-V side of the interior threshold, in raw
    // UV coordinates of the clip texcoord set.
    bool keepGreaterU = false;
    bool keepGreaterV = false;
    // Geographic halves (south-up), used for water mask and skirt metadata.
    bool eastHalf = false;
    bool northHalf = false;
};

ClipSides clipSidesForWindow(const GltfUpsampleUvWindow& window,
                             bool hasInvertedVCoordinate) {
    ClipSides sides;
    sides.keepGreaterU = window.u0 + window.u1 > 1.0;
    sides.keepGreaterV = window.v0 + window.v1 > 1.0;
    sides.eastHalf = sides.keepGreaterU;
    // NW 约定（invertedV=false）：v=0 在北，北半 = 较小的 V
    sides.northHalf = hasInvertedVCoordinate ? sides.keepGreaterV
                                             : !sides.keepGreaterV;
    return sides;
}

float renormalizeCoordinate(float value, double low, double high) {
    const double size = high - low;
    if (!(size > 0.0)) {
        return value;
    }
    return static_cast<float>(
        std::clamp((static_cast<double>(value) - low) / size, 0.0, 1.0));
}

// Maps the kept parent-subrange coordinates onto the child's own [0,1] span.
// cesium-native reaches the same result by dropping the overlay texcoords and
// regenerating them against the child rectangle; texcoords are linear in
// their projected space, so the linear remap is equivalent.
void renormalizeTexCoords(GltfPrimitive& primitive,
                          const GltfUpsampleWindows& windows) {
    const size_t setCount =
        std::min(windows.texCoordSetCount, kGltfMaxTexCoordSets);
    for (size_t set = 0; set < setCount; ++set) {
        const GltfUpsampleUvWindow& window = windows.texCoordSets[set];
        for (std::array<float, 2>& uv : primitive.vertexTexCoords[set]) {
            uv[0] = renormalizeCoordinate(uv[0], window.u0, window.u1);
            uv[1] = renormalizeCoordinate(uv[1], window.v0, window.v1);
        }
    }
    for (SurfaceVertex& vertex : primitive.vertices) {
        vertex.uv[0] = renormalizeCoordinate(
            vertex.uv[0],
            windows.nativeUv.u0,
            windows.nativeUv.u1);
        vertex.uv[1] = renormalizeCoordinate(
            vertex.uv[1],
            windows.nativeUv.v0,
            windows.nativeUv.v1);
    }
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

    // Texcoords are renormalized to the child before skirts are added, so
    // every boundary (parent outer edges and the interior clip line) sits at
    // exactly 0 or 1.
    const double westU = 0.0;
    const double eastU = 1.0;
    // NW 约定（invertedV=false）：v=0 在北
    const double southV = hasInvertedVCoordinate ? 0.0 : 1.0;
    const double northV = hasInvertedVCoordinate ? 1.0 : 0.0;

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
               const ClipSides& sides,
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
        hasInvertedVCoordinate);

    const double shortestHeight = shortestSkirtHeight(parentSkirt);
    primitive.skirtMetadata->skirtWestHeight = sides.eastHalf
        ? shortestHeight * 0.5
        : parentSkirt.skirtWestHeight;
    primitive.skirtMetadata->skirtSouthHeight = sides.northHalf
        ? shortestHeight * 0.5
        : parentSkirt.skirtSouthHeight;
    primitive.skirtMetadata->skirtEastHeight = sides.eastHalf
        ? parentSkirt.skirtEastHeight
        : shortestHeight * 0.5;
    primitive.skirtMetadata->skirtNorthHeight = sides.northHalf
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
    if (!hasInvertedVCoordinate) {
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
    if (!hasInvertedVCoordinate) {
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
                             const GltfUpsampleWindows& windows,
                             const ClipSides& sides,
                             int textureCoordinateIndex) {
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

    const GltfUpsampleUvWindow& clipWindow =
        windows.texCoordSets[static_cast<size_t>(textureCoordinateIndex)];
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
        // Strictly inside the interior boundary, like cesium-native: points
        // exactly on the split line belong to neither child.
        const bool insideU = sides.keepGreaterU ? uv[0] > clipWindow.u0
                                                : uv[0] < clipWindow.u1;
        const bool insideV = sides.keepGreaterV ? uv[1] > clipWindow.v0
                                                : uv[1] < clipWindow.v1;
        if (insideU && insideV) {
            appendPoint(output, parent, index, hasExplicitIndices);
        }
    }

    if (output.vertices.empty() ||
        (hasExplicitIndices && output.indices.empty())) {
        return false;
    }
    renormalizeTexCoords(output, windows);
    return true;
}

bool upsamplePrimitive(const GltfPrimitive& parent,
                       GltfPrimitive& output,
                       const GltfUpsampleWindows& windows,
                       const ClipSides& sides,
                       int textureCoordinateIndex,
                       bool hasInvertedVCoordinate) {
    if (parent.primitiveMode == GltfPrimitiveMode::Points) {
        return upsamplePointsPrimitive(
            parent,
            output,
            windows,
            sides,
            textureCoordinateIndex);
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

    const GltfUpsampleUvWindow& clipWindow =
        windows.texCoordSets[static_cast<size_t>(textureCoordinateIndex)];
    // The interior boundary of the child window is the clip line; the outer
    // boundary coincides with the parent edge, where no geometry lies beyond.
    const double uThreshold =
        sides.keepGreaterU ? clipWindow.u0 : clipWindow.u1;
    const double vThreshold =
        sides.keepGreaterV ? clipWindow.v0 : clipWindow.v1;

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
            sides.keepGreaterU,
            uThreshold);
        polygon = clipPolygon(
            polygon,
            1,
            textureCoordinateIndex,
            sides.keepGreaterV,
            vThreshold);
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

    // Renormalize before skirts so skirt vertices copy child-space texcoords
    // and edge collection sees boundaries at exactly 0/1.
    renormalizeTexCoords(output, windows);

    output.skirtMetadata = parent.skirtMetadata;
    addSkirts(output, sides, textureCoordinateIndex, hasInvertedVCoordinate);

    // Upsampled tiles inherit the parent's per-vertex geomorph heightDelta via
    // the SurfaceVertex copies above, but their clipped/interpolated vertices do
    // not align with this tile's own regular grid — morphing that mismatched
    // delta corrugates the surface (vertical combing on steep slopes). Upsampled
    // tiles are a subdivided copy of the parent surface (no new coarse-self to
    // morph from) and appear near-field where the SSE morph factor is ~1 anyway,
    // so zero the delta: they render un-morphed, continuous with the parent.
    for (SurfaceVertex& vertex : output.vertices) {
        vertex.geomorphHeightDelta = 0.0f;
    }
    return true;
}

// Water mask windows stay 0.5-quadrant based (cesium-native scaleWaterMask
// semantics). The shader applies a single scalar scale, so the tiny V offset
// between a projected mercator midpoint and 0.5 cannot be expressed anyway.
void scaleWaterMask(WaterMask& waterMask, const ClipSides& sides) {
    waterMask.scale *= 0.5;
    waterMask.translationX += waterMask.scale * (sides.eastHalf ? 1.0 : 0.0);
    waterMask.translationY += waterMask.scale * (sides.northHalf ? 1.0 : 0.0);
}

void scalePrimitiveWaterMask(GltfPrimitive& primitive,
                             const GltfPrimitive& parent,
                             const ClipSides& sides) {
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
        primitive.terrainWaterMaskScale * (sides.eastHalf ? 1.0 : 0.0);
    primitive.terrainWaterMaskTranslationY =
        parent.terrainWaterMaskTranslationY +
        primitive.terrainWaterMaskScale * (sides.northHalf ? 1.0 : 0.0);
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
                inverseNodeTransform.transformVector(vertex.normalEcef);
        }
    }
}

/// 校验父网格纹理坐标的 V 轴约定(contracts::Id::TexcoordNwOrigin)。
///
/// 做法:在给定纹理坐标集里找出 v 最小与 v 最大的两个顶点,把它们的 ECEF 位置转成
/// 纬度比一下。NW 约定下 v 最小的那个必须更靠北。只转两个顶点,扫描是 O(n) 浮点
/// 比较,每次上采样一次(非逐帧逐顶点),开销可忽略。
///
/// 跳过条件:v 展布过小(< kMinSpread)时方向本身没有信息量,不判 —— 与其在退化
/// 数据上给假阳性,不如沉默。裙墙顶点挂在边缘下方但沿用边缘 UV,不破坏该关系。
void verifyParentTexcoordConvention(const GltfModel& parentModel,
                                    int textureCoordinateIndex,
                                    bool hasInvertedVCoordinate,
                                    const UpsampledQuadtreeNode& childID) {
    constexpr float kMinSpread = 0.1f;
    const size_t setIndex = static_cast<size_t>(textureCoordinateIndex);

    for (const GltfPrimitive& primitive : parentModel.primitives) {
        const std::vector<std::array<float, 2>>& uvs =
            primitive.vertexTexCoords[setIndex];
        if (uvs.size() != primitive.vertices.size() || uvs.size() < 2) {
            continue;
        }
        size_t minIndex = 0;
        size_t maxIndex = 0;
        for (size_t i = 1; i < uvs.size(); ++i) {
            if (uvs[i][1] < uvs[minIndex][1]) minIndex = i;
            if (uvs[i][1] > uvs[maxIndex][1]) maxIndex = i;
        }
        const float spread = uvs[maxIndex][1] - uvs[minIndex][1];
        if (spread < kMinSpread) continue;

        const Ellipsoid& ellipsoid = Ellipsoid::WGS84();
        const double latAtMinV = ellipsoid
            .cartesianToCartographic(primitive.vertices[minIndex].positionEcef)
            .latitude();
        const double latAtMaxV = ellipsoid
            .cartesianToCartographic(primitive.vertices[maxIndex].positionEcef)
            .latitude();
        // NW:v 小 = 北 = 纬度大。inverted:反之。
        const bool holds = hasInvertedVCoordinate ? (latAtMinV <= latAtMaxV)
                                                  : (latAtMinV >= latAtMaxV);
        GE_CONTRACT(contracts::Id::TexcoordNwOrigin,
                    holds,
                    "child=%d/%d/%d set=%d invertedV=%d "
                    "latAtMinV=%.6f latAtMaxV=%.6f vSpread=%.3f verts=%zu",
                    childID.tileID.z, childID.tileID.x, childID.tileID.y,
                    textureCoordinateIndex,
                    hasInvertedVCoordinate ? 1 : 0,
                    latAtMinV, latAtMaxV,
                    static_cast<double>(spread),
                    uvs.size());
        return;  // 一个有效图元足以判定整个父网格的约定
    }
}

} // namespace

GltfUpsampleUvWindow GltfTerrainUpsampler::quadrantUvWindow(
    const UpsampledQuadtreeNode& childID,
    bool hasInvertedVCoordinate) {
    GltfUpsampleUvWindow window;
    const bool east = childKeepsEast(childID);
    const bool north = childKeepsNorth(childID);
    // NW 约定（invertedV=false）：v=0 在北，北半 = 较小的 V
    const bool greaterV = hasInvertedVCoordinate ? north : !north;
    window.u0 = east ? 0.5 : 0.0;
    window.u1 = east ? 1.0 : 0.5;
    window.v0 = greaterV ? 0.5 : 0.0;
    window.v1 = greaterV ? 1.0 : 0.5;
    return window;
}

std::unique_ptr<GltfModel> GltfTerrainUpsampler::upsampleForRasterOverlay(
    const GltfModel& parentModel,
    const UpsampledQuadtreeNode& childID,
    int textureCoordinateIndex,
    bool hasInvertedVCoordinate,
    const GltfUpsampleWindows* windows) {
    if (textureCoordinateIndex < 0 ||
        textureCoordinateIndex >= static_cast<int>(kGltfMaxTexCoordSets)) {
        return nullptr;
    }

    // 契约(消费侧):下面按 v 轴方向把父网格切成子窗,前提是父网格的纹理坐标
    // 遵循声明的 V 约定(NW = v0 在北;hasInvertedVCoordinate 时相反)。父网格若
    // 来自另一条按相反约定发 UV 的路径,切窗会上下翻,影像整片贴反。
    //
    // 谓词与切窗逻辑数据独立:拿**顶点纬度**和**顶点 v** 两条独立数据流互相印证,
    // 而不是重算一遍 quadrantUvWindow 刚算过的量 —— 两边一起改错时也能拦住。
    // 该约定目前散在 6 处独立注释里各自声明,形态与 winding 收归前完全一致。
    verifyParentTexcoordConvention(parentModel,
                                   textureCoordinateIndex,
                                   hasInvertedVCoordinate,
                                   childID);

    GltfUpsampleWindows effectiveWindows;
    if (windows) {
        effectiveWindows = *windows;
    } else {
        effectiveWindows.texCoordSetCount = kGltfMaxTexCoordSets;
        for (GltfUpsampleUvWindow& setWindow :
             effectiveWindows.texCoordSets) {
            setWindow = quadrantUvWindow(childID, hasInvertedVCoordinate);
        }
        // 原生 uv 同为 NW 约定（QuantizedMeshParser 解码时已翻转）
        effectiveWindows.nativeUv = quadrantUvWindow(childID, false);
    }

    const ClipSides sides = clipSidesForWindow(
        effectiveWindows.texCoordSets[
            static_cast<size_t>(textureCoordinateIndex)],
        hasInvertedVCoordinate);

    // 只拷父模型的**非-primitive 元数据**(与原「深拷全部再 clear primitives」逐字段等价)。
    // 原写法深拷父的 primitives(顶点/索引 CPU 数据,每次子瓦物化 ~百KB-1MB)再立即丢弃,
    // 而下方循环从 parentModel.primitives 重建、从不读 result 里那份拷贝 → 纯浪费。
    // ⚠️ 与 GltfModel 字段保持同步:新增非-primitive 字段须在此补一行(primitives 除外);
    //   terrainWaterMask 由紧接的下一行处理。
    auto result = std::make_unique<GltfModel>();
    result->textures = parentModel.textures;
    result->nodes = parentModel.nodes;
    result->sceneRootNodes = parentModel.sceneRootNodes;
    result->skins = parentModel.skins;
    result->animations = parentModel.animations;
    result->credits = parentModel.credits;
    result->rasterOverlayDetails = parentModel.rasterOverlayDetails;
    result->terrainWaterMaskTextureIndex = parentModel.terrainWaterMaskTextureIndex;
    result->preferredLocalOriginEcef = parentModel.preferredLocalOriginEcef;
    result->activeAnimationIndex = parentModel.activeAnimationIndex;
    result->animationLooping = parentModel.animationLooping;
    result->animationPaused = parentModel.animationPaused;
    result->animationRevision = parentModel.animationRevision;
    result->lastAnimationTimeSeconds = parentModel.lastAnimationTimeSeconds;
    result->terrainWaterMask = parentModel.terrainWaterMask;
    scaleWaterMask(result->terrainWaterMask, sides);

    for (const GltfPrimitive& primitive : parentModel.primitives) {
        GltfPrimitive upsampled;
        if (upsamplePrimitive(
                primitive,
                upsampled,
                effectiveWindows,
                sides,
                textureCoordinateIndex,
                hasInvertedVCoordinate)) {
            scalePrimitiveWaterMask(upsampled, primitive, sides);
            rebuildRuntimeBaseVerticesForNode(upsampled, *result);
            result->primitives.push_back(std::move(upsampled));
        }
    }

    return result->primitives.empty() ? nullptr : std::move(result);
}

} // namespace earth_engine
