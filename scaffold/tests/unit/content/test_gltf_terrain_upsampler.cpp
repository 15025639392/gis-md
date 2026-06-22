#include <gtest/gtest.h>

#include "earth_engine/content/GltfTerrainUpsampler.h"

#include <cmath>

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
    return model;
}

GltfModel makeParentModelWithNodeRuntime(const Vec3& origin) {
    GltfModel model = makeParentModel();
    GltfNodeRuntime rootNode;
    rootNode.baseLocalTransform = Mat4::translation(origin);
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

void expectArrayNear(const std::array<float, 4>& actual,
                     const std::array<float, 4>& expected) {
    for (size_t i = 0; i < actual.size(); ++i) {
        EXPECT_NEAR(expected[i], actual[i], 1e-6f);
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
    for (const SurfaceVertex& vertex : primitive.vertices) {
        EXPECT_LE(vertex.uv[0], 0.5f);
        EXPECT_LE(vertex.uv[1], 0.5f);
    }
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
    for (const SurfaceVertex& vertex : primitive.vertices) {
        EXPECT_GE(vertex.uv[0], 0.5f);
        EXPECT_GE(vertex.uv[1], 0.5f);
    }
    EXPECT_DOUBLE_EQ(0.5, upsampled->terrainWaterMask.translationX);
    EXPECT_DOUBLE_EQ(0.375, upsampled->terrainWaterMask.translationY);
    EXPECT_DOUBLE_EQ(0.25, upsampled->terrainWaterMask.scale);
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
    for (const SurfaceVertex& vertex : primitive.vertices) {
        EXPECT_LE(vertex.uv[0], 0.5f);
        EXPECT_LE(vertex.uv[1], 0.5f);
    }
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
        if (std::abs(vertex.uv[0] - 0.5f) < 1e-6f &&
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
    for (GltfPrimitive& primitive : parent.primitives) {
        for (size_t i = 0; i < primitive.vertices.size(); ++i) {
            primitive.vertices[i].uv[1] = 1.0f - primitive.vertices[i].uv[1];
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
    for (const SurfaceVertex& vertex : primitive.vertices) {
        EXPECT_LE(vertex.uv[0], 0.5f);
        EXPECT_GE(vertex.uv[1], 0.5f);
    }
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
