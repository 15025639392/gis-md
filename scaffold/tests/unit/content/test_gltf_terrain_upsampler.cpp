#include <gtest/gtest.h>

#include "earth_engine/content/GltfTerrainUpsampler.h"

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

} // namespace

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
    EXPECT_EQ(primitive.indices.size(),
              primitive.skirtMetadata->noSkirtIndicesCount);
    EXPECT_EQ(0u, primitive.skirtMetadata->noSkirtVerticesBegin);
    EXPECT_EQ(primitive.vertices.size(),
              primitive.skirtMetadata->noSkirtVerticesCount);
    EXPECT_EQ(parent.primitives.front().skirtMetadata->meshCenter,
              primitive.skirtMetadata->meshCenter);
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
