#include <gtest/gtest.h>

#include "earth_engine/core/geodesy/Ellipsoid.h"
#include "earth_engine/content/GltfTerrainUpsampler.h"

#include <cmath>
#include <limits>
#include <vector>

using namespace earth_engine;

namespace {

SurfaceVertex vertex(double x, double y, double u, double v) {
    SurfaceVertex out;
    out.positionEcef = Vec3(x, y, 0.0);
    out.normalEcef = Vec3::unitZ();
    out.uv = {static_cast<float>(u), static_cast<float>(v)};
    return out;
}

GltfModel makeParentModel() {
    GltfModel model;
    GltfPrimitive primitive;
    primitive.vertices = {
        vertex(0.0, 0.0, 0.0, 0.0),
        vertex(2.0, 0.0, 1.0, 0.0),
        vertex(0.0, 2.0, 0.0, 1.0),
        vertex(2.0, 2.0, 1.0, 1.0)};
    primitive.indices = {0, 1, 2, 1, 3, 2};
    primitive.vertexTexCoords[0] = {
        {0.0f, 0.0f},
        {1.0f, 0.0f},
        {0.0f, 1.0f},
        {1.0f, 1.0f}};
    primitive.runtime.baseVertices = primitive.vertices;
    primitive.runtime.hasNormals = true;
    primitive.hasTerrainWaterMaskMetadata = true;
    primitive.terrainOnlyWater = false;
    primitive.terrainOnlyLand = false;
    primitive.terrainWaterMaskTextureIndex = 0;
    primitive.terrainWaterMaskTranslationX = 0.25;
    primitive.terrainWaterMaskTranslationY = 0.125;
    primitive.terrainWaterMaskScale = 0.5;
    SkirtMetadata skirt;
    skirt.noSkirtIndicesBegin = 0;
    skirt.noSkirtIndicesCount = 6;
    skirt.noSkirtVerticesBegin = 0;
    skirt.noSkirtVerticesCount = 4;
    skirt.meshCenter = Vec3(0.5, 0.5, 0.0);
    skirt.skirtWestHeight = 10.0;
    skirt.skirtSouthHeight = 10.0;
    skirt.skirtEastHeight = 10.0;
    skirt.skirtNorthHeight = 10.0;
    primitive.skirtMetadata = skirt;
    model.primitives.push_back(std::move(primitive));
    model.terrainWaterMask.allLand = false;
    model.terrainWaterMask.allWater = false;
    model.terrainWaterMask.translationX = 0.25;
    model.terrainWaterMask.translationY = 0.125;
    model.terrainWaterMask.scale = 0.5;
    model.terrainWaterMaskTextureIndex = 0;
    model.textures.emplace_back();
    return model;
}

GltfModel makeParentModelWithNodeRuntime(const Vec3& origin) {
    GltfModel model = makeParentModel();
    const Mat4 transform = Mat4::translation(origin);
    GltfNodeRuntime rootNode;
    rootNode.baseLocalTransform = transform;
    rootNode.localTransform = rootNode.baseLocalTransform;
    rootNode.globalTransform = rootNode.baseLocalTransform;
    rootNode.baseTranslation = {origin.x(), origin.y(), origin.z()};
    rootNode.translation = rootNode.baseTranslation;
    rootNode.mesh = 0;
    model.nodes.push_back(rootNode);
    model.sceneRootNodes.push_back(0);
    model.preferredLocalOriginEcef = origin;

    GltfPrimitive& primitive = model.primitives.front();
    primitive.runtime.nodeIndex = 0;
    primitive.runtime.baseVertices = primitive.vertices;
    for (SurfaceVertex& baseVertex : primitive.runtime.baseVertices) {
        baseVertex.positionEcef = baseVertex.positionEcef - origin;
    }
    return model;
}

GltfModel makeParentModelWithNodeRuntimeTransform(const Mat4& transform) {
    GltfModel model = makeParentModel();
    GltfNodeRuntime rootNode;
    rootNode.baseLocalTransform = transform;
    rootNode.localTransform = rootNode.baseLocalTransform;
    rootNode.globalTransform = rootNode.baseLocalTransform;
    rootNode.mesh = 0;
    model.nodes.push_back(rootNode);
    model.sceneRootNodes.push_back(0);

    GltfPrimitive& primitive = model.primitives.front();
    primitive.runtime.nodeIndex = 0;
    primitive.runtime.baseVertices = primitive.vertices;
    const Mat4 inverseTransform = transform.inverse();
    for (SurfaceVertex& baseVertex : primitive.runtime.baseVertices) {
        baseVertex.positionEcef =
            inverseTransform.transformPoint(baseVertex.positionEcef);
        baseVertex.normalEcef =
            inverseTransform.transformVector(baseVertex.normalEcef);
    }
    return model;
}

void expectArrayNear(const std::array<float, 4>& actual,
                     const std::array<float, 4>& expected) {
    for (size_t i = 0; i < actual.size(); ++i) {
        EXPECT_NEAR(expected[i], actual[i], 1e-6f);
    }
}

bool sameUv(const SurfaceVertex& a, const SurfaceVertex& b) {
    return std::abs(a.uv[0] - b.uv[0]) <= 1e-6f &&
           std::abs(a.uv[1] - b.uv[1]) <= 1e-6f;
}

// New contract: kept texcoords are renormalized onto the child's own [0,1]
// span (cesium-native regenerates them against the child rectangle; the
// linear remap is equivalent). A child clipped from a full-tile parent must
// therefore span the whole unit square.
void expectCoordsSpanUnitSquare(
    const std::vector<std::array<float, 2>>& coords,
    size_t count) {
    float minU = 1.0f, maxU = 0.0f, minV = 1.0f, maxV = 0.0f;
    for (size_t i = 0; i < count && i < coords.size(); ++i) {
        EXPECT_GE(coords[i][0], -1e-6f);
        EXPECT_LE(coords[i][0], 1.0f + 1e-6f);
        EXPECT_GE(coords[i][1], -1e-6f);
        EXPECT_LE(coords[i][1], 1.0f + 1e-6f);
        minU = std::min(minU, coords[i][0]);
        maxU = std::max(maxU, coords[i][0]);
        minV = std::min(minV, coords[i][1]);
        maxV = std::max(maxV, coords[i][1]);
    }
    EXPECT_NEAR(0.0f, minU, 1e-6f);
    EXPECT_NEAR(1.0f, maxU, 1e-6f);
    EXPECT_NEAR(0.0f, minV, 1e-6f);
    EXPECT_NEAR(1.0f, maxV, 1e-6f);
}

void expectUvSpansUnitSquare(const GltfPrimitive& primitive) {
    const size_t vertexCount = primitive.skirtMetadata
        ? primitive.skirtMetadata->noSkirtVerticesCount
        : primitive.vertices.size();
    std::vector<std::array<float, 2>> nativeUvs;
    nativeUvs.reserve(vertexCount);
    for (size_t i = 0; i < vertexCount && i < primitive.vertices.size();
         ++i) {
        nativeUvs.push_back(primitive.vertices[i].uv);
    }
    expectCoordsSpanUnitSquare(nativeUvs, vertexCount);
    if (primitive.vertexTexCoords[0].size() == primitive.vertices.size()) {
        expectCoordsSpanUnitSquare(primitive.vertexTexCoords[0], vertexCount);
    }
}

void expectRasterOverlaySkirtsMatchCesiumNative(const GltfPrimitive& primitive,
                                                const SkirtMetadata& skirt) {
    ASSERT_TRUE(primitive.skirtMetadata.has_value());
    const SkirtMetadata& currentSkirt = *primitive.skirtMetadata;
    ASSERT_GT(currentSkirt.noSkirtVerticesCount, 0u);
    ASSERT_LE(
        currentSkirt.noSkirtVerticesBegin + currentSkirt.noSkirtVerticesCount,
        primitive.vertices.size());

    const double westU = std::numeric_limits<double>::infinity();
    const double eastU = -std::numeric_limits<double>::infinity();
    const double southV = std::numeric_limits<double>::infinity();
    const double northV = -std::numeric_limits<double>::infinity();
    double minU = westU;
    double maxU = eastU;
    double minV = southV;
    double maxV = northV;
    const size_t topBegin = currentSkirt.noSkirtVerticesBegin;
    const size_t topEnd = topBegin + currentSkirt.noSkirtVerticesCount;
    for (size_t i = topBegin; i < topEnd; ++i) {
        const SurfaceVertex& vertex = primitive.vertices[i];
        minU = std::min(minU, static_cast<double>(vertex.uv[0]));
        maxU = std::max(maxU, static_cast<double>(vertex.uv[0]));
        minV = std::min(minV, static_cast<double>(vertex.uv[1]));
        maxV = std::max(maxV, static_cast<double>(vertex.uv[1]));
    }

    auto edgeHeightsFor = [&](const SurfaceVertex& vertex) {
        std::vector<double> heights;
        if (std::abs(static_cast<double>(vertex.uv[0]) - minU) <= 1e-5) {
            heights.push_back(currentSkirt.skirtWestHeight);
        }
        if (std::abs(static_cast<double>(vertex.uv[0]) - maxU) <= 1e-5) {
            heights.push_back(currentSkirt.skirtEastHeight);
        }
        if (std::abs(static_cast<double>(vertex.uv[1]) - minV) <= 1e-5) {
            heights.push_back(currentSkirt.skirtSouthHeight);
        }
        if (std::abs(static_cast<double>(vertex.uv[1]) - maxV) <= 1e-5) {
            heights.push_back(currentSkirt.skirtNorthHeight);
        }
        return heights;
    };

    ASSERT_GT(primitive.vertices.size(), topEnd);
    for (size_t i = topEnd; i < primitive.vertices.size(); ++i) {
        const SurfaceVertex& skirtVertex = primitive.vertices[i];
        std::vector<double> candidateHeights = edgeHeightsFor(skirtVertex);
        ASSERT_FALSE(candidateHeights.empty());

        const SurfaceVertex* matchingTop = nullptr;
        for (size_t topIndex = topBegin; topIndex < topEnd; ++topIndex) {
            if (sameUv(primitive.vertices[topIndex], skirtVertex)) {
                matchingTop = &primitive.vertices[topIndex];
                break;
            }
        }
        ASSERT_NE(nullptr, matchingTop);

        bool matchedHeight = false;
        for (double height : candidateHeights) {
            const Ellipsoid ellipsoid = Ellipsoid::WGS84();
            const Vec3 absoluteTop = matchingTop->positionEcef + skirt.meshCenter;
            Vec3 normal = ellipsoid.geodeticSurfaceNormal(absoluteTop);
            if (normal.lengthSquared() <= 0.0 &&
                matchingTop->normalEcef.lengthSquared() > 0.0) {
                normal = matchingTop->normalEcef.normalized();
            }
            ASSERT_GT(normal.lengthSquared(), 0.0);
            normal = normal.normalized();
            const Vec3 expected =
                absoluteTop - normal * height - skirt.meshCenter;
            if ((expected - skirtVertex.positionEcef).length() <= 1e-6) {
                matchedHeight = true;
                break;
            }
        }
        EXPECT_TRUE(matchedHeight);
    }
}

} // namespace

TEST(GltfTerrainUpsamplerTest,
     PreservesNodeLocalRuntimeVerticesLikeCesiumNative) {
    const Vec3 origin(10.0, 20.0, 30.0);
    GltfModel parent = makeParentModelWithNodeRuntime(origin);
    const UpsampledQuadtreeNode child{TileKey{"Geographic-TMS", 1, 0, 0}};

    std::unique_ptr<GltfModel> upsampled =
        GltfTerrainUpsampler::upsampleForRasterOverlay(parent, child, 0, false);

    ASSERT_NE(nullptr, upsampled);
    ASSERT_EQ(1u, upsampled->nodes.size());
    ASSERT_EQ(1u, upsampled->sceneRootNodes.size());
    EXPECT_EQ(0, upsampled->sceneRootNodes.front());
    EXPECT_EQ(Mat4::translation(origin),
              upsampled->nodes.front().globalTransform);
    ASSERT_EQ(1u, upsampled->primitives.size());
    const GltfPrimitive& primitive = upsampled->primitives.front();
    EXPECT_EQ(0, primitive.runtime.nodeIndex);
    ASSERT_FALSE(primitive.vertices.empty());
    ASSERT_EQ(primitive.vertices.size(), primitive.runtime.baseVertices.size());
    for (size_t i = 0; i < primitive.vertices.size(); ++i) {
        EXPECT_LT((primitive.runtime.baseVertices[i].positionEcef -
                   (primitive.vertices[i].positionEcef - origin))
                      .length(),
                  1e-9);
        EXPECT_EQ(primitive.vertices[i].normalEcef,
                  primitive.runtime.baseVertices[i].normalEcef);
    }
}

TEST(GltfTerrainUpsamplerTest,
     PreservesNodeLocalRuntimeNormalScaleLikeCesiumNative) {
    const Mat4 transform =
        Mat4::translation(Vec3(10.0, 20.0, 30.0)) *
        Mat4::scale(Vec3(2.0, 3.0, 4.0));
    GltfModel parent = makeParentModelWithNodeRuntimeTransform(transform);
    GltfPrimitive& parentPrimitive = parent.primitives.front();
    parentPrimitive.skirtMetadata.reset();
    for (SurfaceVertex& vertex : parentPrimitive.vertices) {
        vertex.normalEcef = Vec3::unitX();
    }

    const UpsampledQuadtreeNode child{TileKey{"Geographic-TMS", 1, 0, 0}};

    std::unique_ptr<GltfModel> upsampled =
        GltfTerrainUpsampler::upsampleForRasterOverlay(parent, child, 0, false);

    ASSERT_NE(nullptr, upsampled);
    ASSERT_EQ(1u, upsampled->primitives.size());
    const GltfPrimitive& primitive = upsampled->primitives.front();
    ASSERT_FALSE(primitive.vertices.empty());
    ASSERT_EQ(primitive.vertices.size(), primitive.runtime.baseVertices.size());
    const Mat4 inverseTransform = transform.inverse();
    for (size_t i = 0; i < primitive.vertices.size(); ++i) {
        const Vec3 expectedNormal =
            inverseTransform.transformVector(primitive.vertices[i].normalEcef);
        EXPECT_NEAR(expectedNormal.x(),
                    primitive.runtime.baseVertices[i].normalEcef.x(),
                    1e-12);
        EXPECT_NEAR(expectedNormal.y(),
                    primitive.runtime.baseVertices[i].normalEcef.y(),
                    1e-12);
        EXPECT_NEAR(expectedNormal.z(),
                    primitive.runtime.baseVertices[i].normalEcef.z(),
                    1e-12);
        EXPECT_NEAR(0.5,
                    primitive.runtime.baseVertices[i].normalEcef.length(),
                    1e-12);
    }
}

TEST(GltfTerrainUpsamplerTest,
     ClipsLowerLeftTriangleAtRasterOverlayMidlinesLikeCesiumNative) {
    GltfModel parent = makeParentModel();
    const UpsampledQuadtreeNode child{TileKey{"Geographic-TMS", 1, 0, 0}};

    std::unique_ptr<GltfModel> upsampled =
        GltfTerrainUpsampler::upsampleForRasterOverlay(parent, child, 0, false);

    ASSERT_NE(nullptr, upsampled);
    ASSERT_EQ(1u, upsampled->primitives.size());
    const GltfPrimitive& primitive = upsampled->primitives.front();
    ASSERT_FALSE(primitive.vertices.empty());
    ASSERT_FALSE(primitive.indices.empty());
    expectUvSpansUnitSquare(primitive);
    ASSERT_TRUE(primitive.skirtMetadata.has_value());
    EXPECT_EQ(0u, primitive.skirtMetadata->noSkirtIndicesBegin);
    EXPECT_EQ(0u, primitive.skirtMetadata->noSkirtVerticesBegin);
    EXPECT_LT(
        primitive.skirtMetadata->noSkirtIndicesCount,
        primitive.indices.size());
    EXPECT_LT(
        primitive.skirtMetadata->noSkirtVerticesCount,
        primitive.vertices.size());
    EXPECT_EQ(parent.primitives.front().skirtMetadata->meshCenter,
              primitive.skirtMetadata->meshCenter);
    EXPECT_DOUBLE_EQ(
        10.0,
        primitive.skirtMetadata->skirtWestHeight);
    EXPECT_DOUBLE_EQ(
        10.0,
        primitive.skirtMetadata->skirtSouthHeight);
    EXPECT_DOUBLE_EQ(
        5.0,
        primitive.skirtMetadata->skirtEastHeight);
    EXPECT_DOUBLE_EQ(
        5.0,
        primitive.skirtMetadata->skirtNorthHeight);
    EXPECT_DOUBLE_EQ(0.25, upsampled->terrainWaterMask.translationX);
    EXPECT_DOUBLE_EQ(0.125, upsampled->terrainWaterMask.translationY);
    EXPECT_DOUBLE_EQ(0.25, upsampled->terrainWaterMask.scale);
    ASSERT_TRUE(primitive.terrainWaterMaskTextureIndex.has_value());
    EXPECT_EQ(0u, *primitive.terrainWaterMaskTextureIndex);
    EXPECT_DOUBLE_EQ(0.25, primitive.terrainWaterMaskTranslationX);
    EXPECT_DOUBLE_EQ(0.125, primitive.terrainWaterMaskTranslationY);
    EXPECT_DOUBLE_EQ(0.25, primitive.terrainWaterMaskScale);
}

TEST(GltfTerrainUpsamplerTest,
     ClipsUpperRightTriangleAndOffsetsWaterMaskLikeCesiumNative) {
    GltfModel parent = makeParentModel();
    const UpsampledQuadtreeNode child{TileKey{"Geographic-TMS", 1, 1, 1}};

    std::unique_ptr<GltfModel> upsampled =
        GltfTerrainUpsampler::upsampleForRasterOverlay(parent, child, 0, false);

    ASSERT_NE(nullptr, upsampled);
    ASSERT_EQ(1u, upsampled->primitives.size());
    const GltfPrimitive& primitive = upsampled->primitives.front();
    ASSERT_FALSE(primitive.vertices.empty());
    ASSERT_FALSE(primitive.indices.empty());
    expectUvSpansUnitSquare(primitive);
    EXPECT_DOUBLE_EQ(0.5, upsampled->terrainWaterMask.translationX);
    EXPECT_DOUBLE_EQ(0.375, upsampled->terrainWaterMask.translationY);
    EXPECT_DOUBLE_EQ(0.25, upsampled->terrainWaterMask.scale);
    EXPECT_DOUBLE_EQ(0.5, primitive.terrainWaterMaskTranslationX);
    EXPECT_DOUBLE_EQ(0.375, primitive.terrainWaterMaskTranslationY);
    EXPECT_DOUBLE_EQ(0.25, primitive.terrainWaterMaskScale);
}

TEST(GltfTerrainUpsamplerTest,
     DropsRasterOverlaySkirtsAlongGeodeticNormalForEveryQuadrantLikeCesiumNative) {
    const GltfModel parent = makeParentModel();
    const SkirtMetadata parentSkirt =
        *parent.primitives.front().skirtMetadata;
    const std::vector<UpsampledQuadtreeNode> children = {
        UpsampledQuadtreeNode{TileKey{"Geographic-TMS", 1, 0, 0}},
        UpsampledQuadtreeNode{TileKey{"Geographic-TMS", 1, 1, 0}},
        UpsampledQuadtreeNode{TileKey{"Geographic-TMS", 1, 0, 1}},
        UpsampledQuadtreeNode{TileKey{"Geographic-TMS", 1, 1, 1}}};

    for (const UpsampledQuadtreeNode& child : children) {
        std::unique_ptr<GltfModel> upsampled =
            GltfTerrainUpsampler::upsampleForRasterOverlay(
                parent,
                child,
                0,
                false);

        ASSERT_NE(nullptr, upsampled);
        ASSERT_EQ(1u, upsampled->primitives.size());
        expectRasterOverlaySkirtsMatchCesiumNative(
            upsampled->primitives.front(),
            parentSkirt);
    }
}

TEST(GltfTerrainUpsamplerTest,
     ScalesWaterMaskFromEachPrimitiveExtrasLikeCesiumNative) {
    GltfModel parent = makeParentModel();
    GltfPrimitive mixed = parent.primitives.front();

    parent.primitives.front().hasTerrainWaterMaskMetadata = true;
    parent.primitives.front().terrainOnlyWater = true;
    parent.primitives.front().terrainOnlyLand = false;
    parent.primitives.front().terrainWaterMaskTextureIndex = 0;
    parent.primitives.front().terrainWaterMaskTranslationX = 0.0;
    parent.primitives.front().terrainWaterMaskTranslationY = 0.0;
    parent.primitives.front().terrainWaterMaskScale = 1.0;

    mixed.hasTerrainWaterMaskMetadata = true;
    mixed.terrainOnlyWater = false;
    mixed.terrainOnlyLand = false;
    mixed.terrainWaterMaskTextureIndex = 0;
    mixed.terrainWaterMaskTranslationX = 0.2;
    mixed.terrainWaterMaskTranslationY = 0.4;
    mixed.terrainWaterMaskScale = 0.25;
    parent.primitives.push_back(std::move(mixed));

    parent.terrainWaterMask.translationX = 0.75;
    parent.terrainWaterMask.translationY = 0.75;
    parent.terrainWaterMask.scale = 0.125;

    const UpsampledQuadtreeNode child{TileKey{"Geographic-TMS", 1, 1, 0}};

    std::unique_ptr<GltfModel> upsampled =
        GltfTerrainUpsampler::upsampleForRasterOverlay(parent, child, 0, false);

    ASSERT_NE(nullptr, upsampled);
    ASSERT_EQ(2u, upsampled->primitives.size());

    const GltfPrimitive& onlyWater = upsampled->primitives[0];
    EXPECT_TRUE(onlyWater.hasTerrainWaterMaskMetadata);
    EXPECT_TRUE(onlyWater.terrainOnlyWater);
    EXPECT_FALSE(onlyWater.terrainOnlyLand);
    EXPECT_FALSE(onlyWater.terrainWaterMaskTextureIndex.has_value());
    EXPECT_DOUBLE_EQ(0.5, onlyWater.terrainWaterMaskTranslationX);
    EXPECT_DOUBLE_EQ(0.0, onlyWater.terrainWaterMaskTranslationY);
    EXPECT_DOUBLE_EQ(0.5, onlyWater.terrainWaterMaskScale);

    const GltfPrimitive& mixedChild = upsampled->primitives[1];
    EXPECT_TRUE(mixedChild.hasTerrainWaterMaskMetadata);
    EXPECT_FALSE(mixedChild.terrainOnlyWater);
    EXPECT_FALSE(mixedChild.terrainOnlyLand);
    ASSERT_TRUE(mixedChild.terrainWaterMaskTextureIndex.has_value());
    EXPECT_EQ(0u, *mixedChild.terrainWaterMaskTextureIndex);
    EXPECT_DOUBLE_EQ(0.325, mixedChild.terrainWaterMaskTranslationX);
    EXPECT_DOUBLE_EQ(0.4, mixedChild.terrainWaterMaskTranslationY);
    EXPECT_DOUBLE_EQ(0.125, mixedChild.terrainWaterMaskScale);
}

TEST(GltfTerrainUpsamplerTest,
     MissingWaterMaskExtrasUpsamplesToCesiumNativeDefaultOnlyLand) {
    GltfModel parent = makeParentModel();
    GltfPrimitive& primitive = parent.primitives.front();
    primitive.hasTerrainWaterMaskMetadata = false;
    primitive.terrainOnlyWater = false;
    primitive.terrainOnlyLand = true;
    primitive.terrainWaterMaskTextureIndex.reset();
    primitive.terrainWaterMaskTranslationX = 0.0;
    primitive.terrainWaterMaskTranslationY = 0.0;
    primitive.terrainWaterMaskScale = 1.0;

    const UpsampledQuadtreeNode child{TileKey{"Geographic-TMS", 1, 1, 1}};

    std::unique_ptr<GltfModel> upsampled =
        GltfTerrainUpsampler::upsampleForRasterOverlay(parent, child, 0, false);

    ASSERT_NE(nullptr, upsampled);
    ASSERT_EQ(1u, upsampled->primitives.size());
    const GltfPrimitive& out = upsampled->primitives.front();
    EXPECT_TRUE(out.hasTerrainWaterMaskMetadata);
    EXPECT_FALSE(out.terrainOnlyWater);
    EXPECT_TRUE(out.terrainOnlyLand);
    EXPECT_FALSE(out.terrainWaterMaskTextureIndex.has_value());
    EXPECT_DOUBLE_EQ(0.0, out.terrainWaterMaskTranslationX);
    EXPECT_DOUBLE_EQ(0.0, out.terrainWaterMaskTranslationY);
    EXPECT_DOUBLE_EQ(0.0, out.terrainWaterMaskScale);
}

TEST(GltfTerrainUpsamplerTest,
     ClipsNonIndexedTrianglesLikeCesiumNative) {
    GltfModel parent = makeParentModel();
    GltfPrimitive& parentPrimitive = parent.primitives.front();
    parentPrimitive.indices.clear();
    parentPrimitive.skirtMetadata.reset();
    parentPrimitive.vertices = {
        vertex(0.0, 0.0, 0.0, 0.0),
        vertex(2.0, 0.0, 1.0, 0.0),
        vertex(0.0, 2.0, 0.0, 1.0),
        vertex(0.0, 2.0, 0.0, 1.0),
        vertex(2.0, 0.0, 1.0, 0.0),
        vertex(2.0, 2.0, 1.0, 1.0)};
    parentPrimitive.vertexTexCoords[0] = {
        {0.0f, 0.0f},
        {1.0f, 0.0f},
        {0.0f, 1.0f},
        {0.0f, 1.0f},
        {1.0f, 0.0f},
        {1.0f, 1.0f}};
    parentPrimitive.runtime.baseVertices = parentPrimitive.vertices;

    const UpsampledQuadtreeNode child{TileKey{"Geographic-TMS", 1, 0, 0}};

    std::unique_ptr<GltfModel> upsampled =
        GltfTerrainUpsampler::upsampleForRasterOverlay(parent, child, 0, false);

    ASSERT_NE(nullptr, upsampled);
    ASSERT_EQ(1u, upsampled->primitives.size());
    const GltfPrimitive& primitive = upsampled->primitives.front();
    EXPECT_EQ(GltfPrimitiveMode::Triangles, primitive.primitiveMode);
    ASSERT_FALSE(primitive.vertices.empty());
    ASSERT_FALSE(primitive.indices.empty());
    EXPECT_EQ(0u, primitive.indices.size() % 3u);
    for (uint32_t index : primitive.indices) {
        EXPECT_LT(index, primitive.vertices.size());
    }
    expectUvSpansUnitSquare(primitive);
}

TEST(GltfTerrainUpsamplerTest,
     ClipsTriangleStripPrimitiveLikeCesiumNative) {
    GltfModel parent = makeParentModel();
    GltfPrimitive& primitive = parent.primitives.front();
    primitive.skirtMetadata.reset();
    primitive.primitiveMode = GltfPrimitiveMode::TriangleStrip;
    primitive.indices = {0, 2, 1, 3};

    const UpsampledQuadtreeNode child{TileKey{"Geographic-TMS", 1, 0, 0}};

    std::unique_ptr<GltfModel> upsampled =
        GltfTerrainUpsampler::upsampleForRasterOverlay(parent, child, 0, false);

    ASSERT_NE(nullptr, upsampled);
    ASSERT_EQ(1u, upsampled->primitives.size());
    const GltfPrimitive& out = upsampled->primitives.front();
    EXPECT_EQ(GltfPrimitiveMode::Triangles, out.primitiveMode);
    ASSERT_FALSE(out.vertices.empty());
    ASSERT_FALSE(out.indices.empty());
    EXPECT_EQ(0u, out.indices.size() % 3u);
    expectUvSpansUnitSquare(out);
}

TEST(GltfTerrainUpsamplerTest,
     ClipsTriangleFanPrimitiveLikeCesiumNative) {
    GltfModel parent = makeParentModel();
    GltfPrimitive& primitive = parent.primitives.front();
    primitive.skirtMetadata.reset();
    primitive.primitiveMode = GltfPrimitiveMode::TriangleFan;
    primitive.indices = {0, 1, 3, 2};

    const UpsampledQuadtreeNode child{TileKey{"Geographic-TMS", 1, 0, 0}};

    std::unique_ptr<GltfModel> upsampled =
        GltfTerrainUpsampler::upsampleForRasterOverlay(parent, child, 0, false);

    ASSERT_NE(nullptr, upsampled);
    ASSERT_EQ(1u, upsampled->primitives.size());
    const GltfPrimitive& out = upsampled->primitives.front();
    EXPECT_EQ(GltfPrimitiveMode::Triangles, out.primitiveMode);
    ASSERT_FALSE(out.vertices.empty());
    ASSERT_FALSE(out.indices.empty());
    EXPECT_EQ(0u, out.indices.size() % 3u);
    expectUvSpansUnitSquare(out);
}

TEST(GltfTerrainUpsamplerTest,
     ReusesSharedOriginalTriangleVerticesLikeCesiumNative) {
    GltfModel parent;
    GltfPrimitive primitive;
    primitive.vertices = {
        vertex(0.0, 0.0, 0.1, 0.1),
        vertex(1.0, 0.0, 0.4, 0.1),
        vertex(0.0, 1.0, 0.1, 0.4),
        vertex(1.0, 1.0, 0.4, 0.4)};
    primitive.vertexTexCoords[0] = {
        {0.1f, 0.1f},
        {0.4f, 0.1f},
        {0.1f, 0.4f},
        {0.4f, 0.4f}};
    primitive.indices = {0, 1, 2, 1, 3, 2};
    primitive.runtime.baseVertices = primitive.vertices;
    parent.primitives.push_back(std::move(primitive));

    const UpsampledQuadtreeNode child{TileKey{"Geographic-TMS", 1, 0, 0}};

    std::unique_ptr<GltfModel> upsampled =
        GltfTerrainUpsampler::upsampleForRasterOverlay(parent, child, 0, false);

    ASSERT_NE(nullptr, upsampled);
    ASSERT_EQ(1u, upsampled->primitives.size());
    const GltfPrimitive& out = upsampled->primitives.front();
    EXPECT_EQ(GltfPrimitiveMode::Triangles, out.primitiveMode);
    ASSERT_EQ(4u, out.vertices.size());
    ASSERT_EQ(6u, out.indices.size());
    EXPECT_EQ((std::vector<uint32_t>{0, 1, 2, 1, 3, 2}), out.indices);
}

TEST(GltfTerrainUpsamplerTest,
     InterpolatesTriangleVertexColorAndTangentLikeCesiumNative) {
    GltfModel parent = makeParentModel();
    GltfPrimitive& parentPrimitive = parent.primitives.front();
    parentPrimitive.skirtMetadata.reset();
    parentPrimitive.vertexColors = {
        {0.0f, 0.0f, 0.0f, 1.0f},
        {1.0f, 0.0f, 0.0f, 1.0f},
        {0.0f, 1.0f, 0.0f, 1.0f},
        {1.0f, 1.0f, 0.0f, 1.0f}};
    parentPrimitive.vertexTangents = {
        {1.0f, 0.0f, 0.0f, 1.0f},
        {0.0f, 1.0f, 0.0f, 1.0f},
        {0.0f, 0.0f, 1.0f, 1.0f},
        {1.0f, 1.0f, 1.0f, 1.0f}};

    const UpsampledQuadtreeNode child{TileKey{"Geographic-TMS", 1, 0, 0}};

    std::unique_ptr<GltfModel> upsampled =
        GltfTerrainUpsampler::upsampleForRasterOverlay(parent, child, 0, false);

    ASSERT_NE(nullptr, upsampled);
    ASSERT_EQ(1u, upsampled->primitives.size());
    const GltfPrimitive& primitive = upsampled->primitives.front();
    ASSERT_EQ(primitive.vertices.size(), primitive.vertexColors.size());
    ASSERT_EQ(primitive.vertices.size(), primitive.vertexTangents.size());

    bool foundEastMidpoint = false;
    for (size_t i = 0; i < primitive.vertices.size(); ++i) {
        const SurfaceVertex& vertex = primitive.vertices[i];
        // The parent-space east midpoint (0.5, 0) renormalizes onto the
        // lower-left child's east edge (1, 0).
        if (std::abs(vertex.uv[0] - 1.0f) < 1e-6f &&
            std::abs(vertex.uv[1]) < 1e-6f) {
            foundEastMidpoint = true;
            expectArrayNear(
                primitive.vertexColors[i],
                {0.5f, 0.0f, 0.0f, 1.0f});
            expectArrayNear(
                primitive.vertexTangents[i],
                {0.5f, 0.5f, 0.0f, 1.0f});
        }
    }
    EXPECT_TRUE(foundEastMidpoint);
}

TEST(GltfTerrainUpsamplerTest,
     InterpolatesTriangleNormalWithoutCpuRenormalizationLikeCesiumNative) {
    GltfModel parent = makeParentModel();
    GltfPrimitive& parentPrimitive = parent.primitives.front();
    parentPrimitive.skirtMetadata.reset();
    parentPrimitive.vertices[0].normalEcef = Vec3(1.0, 0.0, 0.0);
    parentPrimitive.vertices[1].normalEcef = Vec3(0.0, 1.0, 0.0);
    parentPrimitive.vertices[2].normalEcef = Vec3(0.0, 0.0, 1.0);
    parentPrimitive.vertices[3].normalEcef = Vec3(1.0, 1.0, 1.0);

    const UpsampledQuadtreeNode child{TileKey{"Geographic-TMS", 1, 0, 0}};

    std::unique_ptr<GltfModel> upsampled =
        GltfTerrainUpsampler::upsampleForRasterOverlay(parent, child, 0, false);

    ASSERT_NE(nullptr, upsampled);
    ASSERT_EQ(1u, upsampled->primitives.size());
    const GltfPrimitive& primitive = upsampled->primitives.front();

    bool foundEastMidpoint = false;
    for (const SurfaceVertex& vertex : primitive.vertices) {
        // The parent-space east midpoint (0.5, 0) renormalizes onto the
        // lower-left child's east edge (1, 0).
        if (std::abs(vertex.uv[0] - 1.0f) < 1e-6f &&
            std::abs(vertex.uv[1]) < 1e-6f) {
            foundEastMidpoint = true;
            EXPECT_NEAR(0.5, vertex.normalEcef.x(), 1e-12);
            EXPECT_NEAR(0.5, vertex.normalEcef.y(), 1e-12);
            EXPECT_NEAR(0.0, vertex.normalEcef.z(), 1e-12);
            EXPECT_NEAR(std::sqrt(0.5), vertex.normalEcef.length(), 1e-12);
        }
    }
    EXPECT_TRUE(foundEastMidpoint);
}

TEST(GltfTerrainUpsamplerTest,
    PreservesTriangleFeatureMetadataWhenClippingLikeCesiumNative) {
    GltfModel parent = makeParentModel();
    GltfPrimitive& parentPrimitive = parent.primitives.front();
    parentPrimitive.featureIds = {42, 42, 42, 42};
    parentPrimitive.featureProperties.resize(4);
    for (auto& properties : parentPrimitive.featureProperties) {
        properties["name"] = std::string("terrain-feature");
        properties["height"] = uint64_t(123);
    }

    const UpsampledQuadtreeNode child{TileKey{"Geographic-TMS", 1, 0, 0}};

    std::unique_ptr<GltfModel> upsampled =
        GltfTerrainUpsampler::upsampleForRasterOverlay(parent, child, 0, false);

    ASSERT_NE(nullptr, upsampled);
    ASSERT_EQ(1u, upsampled->primitives.size());
    const GltfPrimitive& primitive = upsampled->primitives.front();
    ASSERT_GT(primitive.vertices.size(), parentPrimitive.vertices.size());
    ASSERT_EQ(primitive.vertices.size(), primitive.featureIds.size());
    ASSERT_EQ(primitive.vertices.size(), primitive.featureProperties.size());
    for (size_t i = 0; i < primitive.vertices.size(); ++i) {
        EXPECT_EQ(42u, primitive.featureIds[i]);
        ASSERT_NE(
            primitive.featureProperties[i].end(),
            primitive.featureProperties[i].find("name"));
        ASSERT_NE(
            primitive.featureProperties[i].end(),
            primitive.featureProperties[i].find("height"));
        ASSERT_NE(
            nullptr,
            std::get_if<std::string>(
                &primitive.featureProperties[i].at("name")));
        EXPECT_EQ(
            "terrain-feature",
            *std::get_if<std::string>(
                &primitive.featureProperties[i].at("name")));
        ASSERT_NE(
            nullptr,
            std::get_if<uint64_t>(
                &primitive.featureProperties[i].at("height")));
        EXPECT_EQ(
            123u,
            *std::get_if<uint64_t>(
                &primitive.featureProperties[i].at("height")));
    }
}

TEST(GltfTerrainUpsamplerTest,
     FiltersPointPrimitiveByRasterOverlayQuadrantLikeCesiumNative) {
    GltfModel parent;
    GltfPrimitive primitive;
    primitive.primitiveMode = GltfPrimitiveMode::Points;
    primitive.vertices = {
        vertex(0.0, 0.0, 0.0, 0.0),
        vertex(0.0, 2.0, 0.0, 1.0),
        vertex(2.0, 0.0, 1.0, 0.0),
        vertex(2.0, 2.0, 1.0, 1.0)};
    primitive.vertexTexCoords[0] = {
        {0.0f, 0.0f},
        {0.0f, 1.0f},
        {1.0f, 0.0f},
        {1.0f, 1.0f}};
    primitive.vertexColors = {
        {0.5f, 0.1f, 0.2f, 1.0f},
        {0.1f, 0.2f, 0.3f, 1.0f},
        {0.3f, 0.4f, 0.5f, 1.0f},
        {0.6f, 0.7f, 0.8f, 1.0f}};
    primitive.featureIds = {10, 11, 12, 13};
    primitive.indices = {0, 1, 2, 3};
    primitive.runtime.baseVertices = primitive.vertices;
    parent.primitives.push_back(std::move(primitive));

    const UpsampledQuadtreeNode child{TileKey{"Geographic-TMS", 1, 0, 0}};

    std::unique_ptr<GltfModel> upsampled =
        GltfTerrainUpsampler::upsampleForRasterOverlay(parent, child, 0, false);

    ASSERT_NE(nullptr, upsampled);
    ASSERT_EQ(1u, upsampled->primitives.size());
    const GltfPrimitive& out = upsampled->primitives.front();
    EXPECT_EQ(GltfPrimitiveMode::Points, out.primitiveMode);
    ASSERT_EQ(1u, out.vertices.size());
    ASSERT_EQ(1u, out.indices.size());
    EXPECT_EQ(0u, out.indices.front());
    EXPECT_EQ(parent.primitives.front().vertices[0].positionEcef,
              out.vertices.front().positionEcef);
    ASSERT_EQ(1u, out.vertexColors.size());
    EXPECT_EQ(parent.primitives.front().vertexColors[0],
              out.vertexColors.front());
    ASSERT_EQ(1u, out.featureIds.size());
    EXPECT_EQ(10u, out.featureIds.front());
}

TEST(GltfTerrainUpsamplerTest,
     PreservesNonIndexedPointPrimitiveLikeCesiumNative) {
    GltfModel parent;
    GltfPrimitive primitive;
    primitive.primitiveMode = GltfPrimitiveMode::Points;
    primitive.vertices = {
        vertex(0.0, 0.0, 0.0, 0.0),
        vertex(0.0, 2.0, 0.0, 1.0),
        vertex(2.0, 0.0, 1.0, 0.0),
        vertex(2.0, 2.0, 1.0, 1.0)};
    primitive.vertexTexCoords[0] = {
        {0.0f, 0.0f},
        {0.0f, 1.0f},
        {1.0f, 0.0f},
        {1.0f, 1.0f}};
    primitive.vertexColors = {
        {0.5f, 0.1f, 0.2f, 1.0f},
        {0.1f, 0.2f, 0.3f, 1.0f},
        {0.3f, 0.4f, 0.5f, 1.0f},
        {0.6f, 0.7f, 0.8f, 1.0f}};
    primitive.featureIds = {10, 11, 12, 13};
    primitive.runtime.baseVertices = primitive.vertices;
    parent.primitives.push_back(std::move(primitive));

    const UpsampledQuadtreeNode child{TileKey{"Geographic-TMS", 1, 0, 0}};

    std::unique_ptr<GltfModel> upsampled =
        GltfTerrainUpsampler::upsampleForRasterOverlay(parent, child, 0, false);

    ASSERT_NE(nullptr, upsampled);
    ASSERT_EQ(1u, upsampled->primitives.size());
    const GltfPrimitive& out = upsampled->primitives.front();
    EXPECT_EQ(GltfPrimitiveMode::Points, out.primitiveMode);
    ASSERT_EQ(1u, out.vertices.size());
    EXPECT_TRUE(out.indices.empty());
    EXPECT_EQ(parent.primitives.front().vertices[0].positionEcef,
              out.vertices.front().positionEcef);
    ASSERT_EQ(1u, out.vertexColors.size());
    EXPECT_EQ(parent.primitives.front().vertexColors[0],
              out.vertexColors.front());
    ASSERT_EQ(1u, out.featureIds.size());
    EXPECT_EQ(10u, out.featureIds.front());
}

TEST(GltfTerrainUpsamplerTest,
     DropsPointPrimitiveBoundarySamplesLikeCesiumNative) {
    GltfModel parent;
    GltfPrimitive primitive;
    primitive.primitiveMode = GltfPrimitiveMode::Points;
    primitive.vertices = {
        vertex(0.0, 0.0, 0.25, 0.25),
        vertex(1.0, 0.0, 0.5, 0.25),
        vertex(0.0, 1.0, 0.25, 0.5),
        vertex(1.0, 1.0, 0.5, 0.5)};
    primitive.vertexTexCoords[0] = {
        {0.25f, 0.25f},
        {0.5f, 0.25f},
        {0.25f, 0.5f},
        {0.5f, 0.5f}};
    primitive.featureIds = {7, 8, 9, 10};
    primitive.runtime.baseVertices = primitive.vertices;
    parent.primitives.push_back(std::move(primitive));

    const UpsampledQuadtreeNode child{TileKey{"Geographic-TMS", 1, 0, 0}};

    std::unique_ptr<GltfModel> upsampled =
        GltfTerrainUpsampler::upsampleForRasterOverlay(parent, child, 0, false);

    ASSERT_NE(nullptr, upsampled);
    ASSERT_EQ(1u, upsampled->primitives.size());
    const GltfPrimitive& out = upsampled->primitives.front();
    ASSERT_EQ(1u, out.vertices.size());
    EXPECT_EQ(parent.primitives.front().vertices[0].positionEcef,
              out.vertices.front().positionEcef);
    ASSERT_EQ(1u, out.featureIds.size());
    EXPECT_EQ(7u, out.featureIds.front());
}

TEST(GltfTerrainUpsamplerTest,
     ClipsLowerLeftChildWithInvertedVCoordinateLikeCesiumNative) {
    GltfModel parent = makeParentModel();
    // hasInvertedVCoordinate refers to the overlay texcoord set; the native
    // SurfaceVertex::uv stays south-up like production quantized-mesh data.
    for (GltfPrimitive& primitive : parent.primitives) {
        for (size_t i = 0; i < primitive.vertices.size(); ++i) {
            primitive.vertexTexCoords[0][i][1] =
                1.0f - primitive.vertexTexCoords[0][i][1];
        }
    }
    const UpsampledQuadtreeNode child{TileKey{"Geographic-TMS", 1, 0, 0}};

    std::unique_ptr<GltfModel> upsampled =
        GltfTerrainUpsampler::upsampleForRasterOverlay(parent, child, 0, true);

    ASSERT_NE(nullptr, upsampled);
    ASSERT_EQ(1u, upsampled->primitives.size());
    const GltfPrimitive& primitive = upsampled->primitives.front();
    ASSERT_FALSE(primitive.vertices.empty());
    ASSERT_FALSE(primitive.indices.empty());
    expectUvSpansUnitSquare(primitive);
    EXPECT_DOUBLE_EQ(0.25, upsampled->terrainWaterMask.translationX);
    EXPECT_DOUBLE_EQ(0.125, upsampled->terrainWaterMask.translationY);
    EXPECT_DOUBLE_EQ(0.25, upsampled->terrainWaterMask.scale);
}

TEST(GltfTerrainUpsamplerTest,
     DropsPrimitiveWithoutRequestedRasterOverlayTexCoords) {
    GltfModel parent = makeParentModel();
    parent.primitives.front().vertexTexCoords[0].clear();
    const UpsampledQuadtreeNode child{TileKey{"Geographic-TMS", 1, 0, 0}};

    std::unique_ptr<GltfModel> upsampled =
        GltfTerrainUpsampler::upsampleForRasterOverlay(parent, child, 0, false);

    EXPECT_EQ(nullptr, upsampled);
}

TEST(GltfTerrainUpsamplerTest,
     ClipsAtChildWindowBoundaryAndRenormalizesPerSetWindow) {
    GltfModel parent = makeParentModel();
    parent.primitives.front().skirtMetadata.reset();

    // South-west child whose interior V boundary sits at 0.4 of the parent
    // texcoord space (a WebMercator-style asymmetric split), while the native
    // uv keeps its exact geographic quadrant window.
    GltfUpsampleWindows windows;
    windows.texCoordSetCount = 1;
    windows.texCoordSets[0] = GltfUpsampleUvWindow{0.0, 0.0, 0.5, 0.4};
    windows.nativeUv = GltfUpsampleUvWindow{0.0, 0.0, 0.5, 0.5};

    const UpsampledQuadtreeNode child{TileKey{"Geographic-TMS", 1, 0, 0}};
    std::unique_ptr<GltfModel> upsampled =
        GltfTerrainUpsampler::upsampleForRasterOverlay(
            parent,
            child,
            0,
            false,
            &windows);

    ASSERT_NE(nullptr, upsampled);
    ASSERT_EQ(1u, upsampled->primitives.size());
    const GltfPrimitive& primitive = upsampled->primitives.front();
    ASSERT_FALSE(primitive.vertices.empty());
    ASSERT_EQ(primitive.vertices.size(), primitive.vertexTexCoords[0].size());

    double maxY = 0.0;
    double maxTexU = 0.0;
    double maxTexV = 0.0;
    double maxNativeV = 0.0;
    for (size_t i = 0; i < primitive.vertices.size(); ++i) {
        maxY = std::max(maxY, primitive.vertices[i].positionEcef.y());
        maxTexU = std::max(
            maxTexU,
            static_cast<double>(primitive.vertexTexCoords[0][i][0]));
        maxTexV = std::max(
            maxTexV,
            static_cast<double>(primitive.vertexTexCoords[0][i][1]));
        maxNativeV = std::max(
            maxNativeV,
            static_cast<double>(primitive.vertices[i].uv[1]));
    }
    // The parent quad spans y in [0,2] with texcoord v = y/2; clipping at the
    // window boundary v = 0.4 (not 0.5) keeps y <= 0.8.
    EXPECT_NEAR(0.8, maxY, 1e-6);
    // The kept texcoord subrange [0,0.5]x[0,0.4] renormalizes to the unit
    // square of the clip set's window.
    EXPECT_NEAR(1.0, maxTexU, 1e-6);
    EXPECT_NEAR(1.0, maxTexV, 1e-6);
    // The native uv renormalizes by its own window: kept v in [0,0.4] over a
    // [0,0.5] window becomes [0,0.8].
    EXPECT_NEAR(0.8, maxNativeV, 1e-6);
}

TEST(GltfTerrainUpsamplerTest,
     DerivesKeptHalfFromWindowNotTileKeyParity) {
    GltfModel parent = makeParentModel();
    parent.primitives.front().skirtMetadata.reset();

    // XYZ-style schemes have y = 0 as the NORTH child. The window carries the
    // geometric truth, so the clip must keep the northern half even though
    // TMS parity (even y) would suggest the south half.
    GltfUpsampleWindows windows;
    windows.texCoordSetCount = 1;
    windows.texCoordSets[0] = GltfUpsampleUvWindow{0.5, 0.5, 1.0, 1.0};
    windows.nativeUv = GltfUpsampleUvWindow{0.5, 0.5, 1.0, 1.0};

    const UpsampledQuadtreeNode child{TileKey{"XYZ-WebMercator", 1, 1, 0}};
    std::unique_ptr<GltfModel> upsampled =
        GltfTerrainUpsampler::upsampleForRasterOverlay(
            parent,
            child,
            0,
            false,
            &windows);

    ASSERT_NE(nullptr, upsampled);
    ASSERT_EQ(1u, upsampled->primitives.size());
    const GltfPrimitive& primitive = upsampled->primitives.front();
    ASSERT_FALSE(primitive.vertices.empty());
    double minX = 2.0;
    double minY = 2.0;
    for (const SurfaceVertex& vertex : primitive.vertices) {
        minX = std::min(minX, vertex.positionEcef.x());
        minY = std::min(minY, vertex.positionEcef.y());
    }
    // Keeping u >= 0.5 and v >= 0.5 keeps x >= 1 and y >= 1 of the [0,2] quad.
    EXPECT_NEAR(1.0, minX, 1e-6);
    EXPECT_NEAR(1.0, minY, 1e-6);
    expectUvSpansUnitSquare(primitive);
}
