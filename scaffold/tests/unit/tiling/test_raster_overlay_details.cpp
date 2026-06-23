#include <gtest/gtest.h>

#include <memory>

#include "earth_engine/content/GltfModel.h"
#include "earth_engine/core/geodesy/Cartographic.h"
#include "earth_engine/core/geodesy/Ellipsoid.h"
#include "earth_engine/core/geodesy/Projection.h"
#include "earth_engine/core/math/Rectangle.h"
#include "earth_engine/tiling/SurfaceTile.h"
#include "earth_engine/tiling/TileBoundingVolume.h"
#include "earth_engine/tiling/TileRasterOverlayDetailsGenerator.h"
#include "earth_engine/tiling/TileRenderContentState.h"

using namespace earth_engine;

namespace {

void prepareGltfRenderContent(TileRenderContentState& renderContent,
                              RasterOverlayDetails details = {}) {
    auto model = std::make_unique<GltfModel>();
    model->rasterOverlayDetails = std::move(details);
    renderContent.prepareGltfContent(std::move(model), Mat4::identity());
    renderContent.setTerrainRenderContent(true);
}

std::unique_ptr<GltfModel> makeTerrainQuadModel(const Rectangle& rectangle) {
    auto model = std::make_unique<GltfModel>();
    GltfPrimitive primitive;
    const Ellipsoid& ellipsoid = Ellipsoid::WGS84();
    const std::array<Cartographic, 4> corners = {
        Cartographic::fromRadians(rectangle.west(), rectangle.south(), 0.0),
        Cartographic::fromRadians(rectangle.east(), rectangle.south(), 0.0),
        Cartographic::fromRadians(rectangle.east(), rectangle.north(), 0.0),
        Cartographic::fromRadians(rectangle.west(), rectangle.north(), 0.0)};
    for (const Cartographic& corner : corners) {
        SurfaceVertex vertex;
        vertex.positionEcef = ellipsoid.cartographicToCartesian(corner);
        primitive.vertices.push_back(vertex);
    }
    primitive.indices = {0, 1, 2, 0, 2, 3};
    model->primitives.push_back(std::move(primitive));
    return model;
}

std::unique_ptr<GltfModel> makeRtcTerrainQuadModel(const Rectangle& rectangle) {
    auto model = makeTerrainQuadModel(rectangle);
    const Cartographic center = Cartographic::fromRadians(
        rectangle.west() + rectangle.width() * 0.5,
        rectangle.south() + rectangle.height() * 0.5,
        0.0);
    const Vec3 origin =
        Ellipsoid::WGS84().cartographicToCartesian(center);

    GltfNodeRuntime node;
    node.baseLocalTransform = Mat4::translation(origin);
    node.localTransform = node.baseLocalTransform;
    node.globalTransform = node.baseLocalTransform;
    node.baseTranslation = {origin.x(), origin.y(), origin.z()};
    node.translation = node.baseTranslation;
    node.mesh = 0;
    node.hasMatrix = false;
    model->nodes.push_back(node);
    model->sceneRootNodes.push_back(0);

    GltfPrimitive& primitive = model->primitives.front();
    primitive.runtime.nodeIndex = 0;
    for (SurfaceVertex& vertex : primitive.vertices) {
        vertex.positionEcef = vertex.positionEcef - origin;
    }
    primitive.runtime.baseVertices = primitive.vertices;
    return model;
}

void prepareTerrainQuadRenderContent(TileRenderContentState& renderContent,
                                     const Rectangle& rectangle,
                                     RasterOverlayDetails details = {}) {
    auto model = makeTerrainQuadModel(rectangle);
    model->rasterOverlayDetails = std::move(details);
    renderContent.prepareGltfContent(std::move(model), Mat4::identity());
    renderContent.setTerrainRenderContent(true);
}

void expectRectangleNear(const Rectangle& expected,
                         const Rectangle& actual,
                         double epsilon = 1e-12) {
    EXPECT_NEAR(expected.west(), actual.west(), epsilon);
    EXPECT_NEAR(expected.south(), actual.south(), epsilon);
    EXPECT_NEAR(expected.east(), actual.east(), epsilon);
    EXPECT_NEAR(expected.north(), actual.north(), epsilon);
}

} // namespace

TEST(RasterOverlayDetailsTest, MergeAppendsProjectionRectanglesLikeCesiumNative) {
    RasterOverlayDetails first;
    const Rectangle firstRectangle(0.0, 1.0, 2.0, 3.0);
    first.setGeographicRectangle(firstRectangle, -10.0, 20.0);

    RasterOverlayDetails second;
    const Rectangle secondRectangle(4.0, 5.0, 6.0, 7.0);
    second.setGeographicRectangle(secondRectangle, -30.0, 40.0);
    second.rasterOverlayInvertedVCoordinates = {true};

    first.merge(second);

    ASSERT_EQ(2u, first.rasterOverlayProjections.size());
    ASSERT_EQ(2u, first.rasterOverlayRectangles.size());
    ASSERT_EQ(2u, first.rasterOverlayInvertedVCoordinates.size());
    EXPECT_EQ(RasterOverlayProjection::Geographic,
              first.rasterOverlayProjections[0]);
    EXPECT_EQ(RasterOverlayProjection::Geographic,
              first.rasterOverlayProjections[1]);
    EXPECT_FALSE(first.rasterOverlayInvertedVCoordinates[0]);
    EXPECT_TRUE(first.rasterOverlayInvertedVCoordinates[1]);
    EXPECT_DOUBLE_EQ(firstRectangle.west(),
                     first.rasterOverlayRectangles[0].west());
    EXPECT_DOUBLE_EQ(secondRectangle.west(),
                     first.rasterOverlayRectangles[1].west());

    const Rectangle* found = first.findRectangleForOverlayProjection(
        RasterOverlayProjection::Geographic);
    ASSERT_NE(nullptr, found);
    EXPECT_EQ(found, &first.rasterOverlayRectangles[0]);
    EXPECT_EQ(0, first.textureCoordinateIDForProjection(
                     RasterOverlayProjection::Geographic));
    EXPECT_EQ(firstRectangle.computeUnion(secondRectangle),
              first.boundingRegion.rectangle);
    EXPECT_DOUBLE_EQ(-30.0, first.boundingRegion.minimumHeight);
    EXPECT_DOUBLE_EQ(40.0, first.boundingRegion.maximumHeight);
}

TEST(RasterOverlayDetailsTest,
     SetGeographicRectangleStoresBoundingRegionLikeCesiumNative) {
    RasterOverlayDetails details;
    const Rectangle rectangle(0.25, 0.5, 0.75, 1.0);

    details.setGeographicRectangle(rectangle, -123.0, 456.0);

    EXPECT_EQ(rectangle, details.boundingRegion.rectangle);
    EXPECT_DOUBLE_EQ(-123.0, details.boundingRegion.minimumHeight);
    EXPECT_DOUBLE_EQ(456.0, details.boundingRegion.maximumHeight);
    EXPECT_FALSE(details.hasInvertedVCoordinateForProjection(
        RasterOverlayProjection::Geographic));
}

TEST(RasterOverlayDetailsGeneratorTest,
     RegionGenerationSkipsExistingProjectionSlotLikeCesiumNative) {
    TileRenderContentState renderContent;
    RasterOverlayDetails existingDetails;
    existingDetails.rasterOverlayProjections.push_back(
        RasterOverlayProjection::Geographic);
    prepareGltfRenderContent(renderContent, std::move(existingDetails));

    const Rectangle region = Rectangle::fromDegrees(-12.0, -4.0, -6.0, 2.0);
    const TileBoundingVolume boundingRegion =
        TileBoundingVolume::fromRegion(region, -25.0, 125.0);

    const bool generated =
        TileRasterOverlayDetailsGenerator::ensureProjectionDetailsFromRegion(
            renderContent,
            boundingRegion,
            RasterOverlayProjection::Geographic);

    EXPECT_FALSE(generated);
    const RasterOverlayDetails& details = renderContent.rasterOverlayDetails();
    ASSERT_EQ(1u, details.rasterOverlayProjections.size());
    EXPECT_TRUE(details.rasterOverlayRectangles.empty());
    EXPECT_EQ(-1, details.textureCoordinateIDForProjection(
                     RasterOverlayProjection::Geographic));
    EXPECT_TRUE(details.boundingRegion.rectangle.isEmpty());
    EXPECT_GT(details.boundingRegion.minimumHeight,
              details.boundingRegion.maximumHeight);
}

TEST(RasterOverlayDetailsGeneratorTest,
     RegionGenerationAppendsMissingProjectionAfterExistingSlotLikeCesiumNative) {
    TileRenderContentState renderContent;
    RasterOverlayDetails existingDetails;
    existingDetails.rasterOverlayProjections.push_back(
        RasterOverlayProjection::Geographic);

    const Rectangle region = Rectangle::fromDegrees(-12.0, -4.0, -6.0, 2.0);
    prepareTerrainQuadRenderContent(
        renderContent,
        region,
        std::move(existingDetails));
    const TileBoundingVolume boundingRegion =
        TileBoundingVolume::fromRegion(region, -25.0, 125.0);
    const Rectangle projected = projectRectangleSimple(
        WebMercatorProjection(Ellipsoid::WGS84()),
        region);

    const bool generated =
        TileRasterOverlayDetailsGenerator::ensureProjectionDetailsFromRegion(
            renderContent,
            boundingRegion,
            RasterOverlayProjection::WebMercator);

    EXPECT_TRUE(generated);
    const RasterOverlayDetails& details = renderContent.rasterOverlayDetails();
    ASSERT_EQ(2u, details.rasterOverlayProjections.size());
    ASSERT_EQ(2u, details.rasterOverlayRectangles.size());
    EXPECT_EQ(RasterOverlayProjection::Geographic,
              details.rasterOverlayProjections[0]);
    EXPECT_EQ(RasterOverlayProjection::WebMercator,
              details.rasterOverlayProjections[1]);
    EXPECT_TRUE(details.rasterOverlayRectangles[0].isEmpty());
    EXPECT_EQ(projected, details.rasterOverlayRectangles[1]);
    EXPECT_EQ(1, details.textureCoordinateIDForProjection(
                     RasterOverlayProjection::WebMercator));
    expectRectangleNear(region, details.boundingRegion.rectangle);
    EXPECT_NEAR(0.0, details.boundingRegion.minimumHeight, 1e-6);
    EXPECT_NEAR(0.0, details.boundingRegion.maximumHeight, 1e-6);
}

TEST(RasterOverlayDetailsGeneratorTest,
     RegionGenerationSkipsExistingRectangleLikeCesiumNative) {
    TileRenderContentState renderContent;
    RasterOverlayDetails existingDetails;
    const Rectangle existing = Rectangle::fromDegrees(1.0, 2.0, 3.0, 4.0);
    existingDetails.setGeographicRectangle(existing, 10.0, 20.0);
    prepareGltfRenderContent(renderContent, std::move(existingDetails));

    const TileBoundingVolume boundingRegion = TileBoundingVolume::fromRegion(
        Rectangle::fromDegrees(-12.0, -4.0, -6.0, 2.0),
        -25.0,
        125.0);

    const bool generated =
        TileRasterOverlayDetailsGenerator::ensureProjectionDetailsFromRegion(
            renderContent,
            boundingRegion,
            RasterOverlayProjection::Geographic);

    EXPECT_FALSE(generated);
    const RasterOverlayDetails& details = renderContent.rasterOverlayDetails();
    ASSERT_EQ(1u, details.rasterOverlayProjections.size());
    ASSERT_EQ(1u, details.rasterOverlayRectangles.size());
    EXPECT_EQ(existing, details.rasterOverlayRectangles[0]);
    EXPECT_EQ(0, details.textureCoordinateIDForProjection(
                     RasterOverlayProjection::Geographic));
    EXPECT_DOUBLE_EQ(10.0, details.boundingRegion.minimumHeight);
    EXPECT_DOUBLE_EQ(20.0, details.boundingRegion.maximumHeight);
}

TEST(RasterOverlayDetailsGeneratorTest,
     RegionGenerationProjectsWebMercatorRectangleLikeCesiumNative) {
    TileRenderContentState renderContent;

    const Rectangle region = Rectangle::fromDegrees(-90.0, -45.0, 45.0, 60.0);
    prepareTerrainQuadRenderContent(renderContent, region);
    const TileBoundingVolume boundingRegion =
        TileBoundingVolume::fromRegion(region, -15.0, 250.0);
    const Rectangle projected = projectRectangleSimple(
        WebMercatorProjection(Ellipsoid::WGS84()),
        region);

    const bool generated =
        TileRasterOverlayDetailsGenerator::ensureProjectionDetailsFromRegion(
            renderContent,
            boundingRegion,
            RasterOverlayProjection::WebMercator);

    EXPECT_TRUE(generated);
    const RasterOverlayDetails& details = renderContent.rasterOverlayDetails();
    ASSERT_EQ(1u, details.rasterOverlayProjections.size());
    ASSERT_EQ(1u, details.rasterOverlayRectangles.size());
    EXPECT_EQ(RasterOverlayProjection::WebMercator,
              details.rasterOverlayProjections[0]);
    EXPECT_EQ(projected, details.rasterOverlayRectangles[0]);
    EXPECT_EQ(0, details.textureCoordinateIDForProjection(
                     RasterOverlayProjection::WebMercator));
    EXPECT_EQ(nullptr, details.findRectangleForOverlayProjection(
                           RasterOverlayProjection::Geographic));
    expectRectangleNear(region, details.boundingRegion.rectangle);
    EXPECT_NEAR(0.0, details.boundingRegion.minimumHeight, 1e-6);
    EXPECT_NEAR(0.0, details.boundingRegion.maximumHeight, 1e-6);
}

TEST(RasterOverlayDetailsGeneratorTest,
     RegionGenerationWritesGltfOverlayTexCoordsLikeCesiumNative) {
    TileRenderContentState renderContent;
    const Rectangle region = Rectangle::fromDegrees(-12.0, -4.0, -6.0, 2.0);
    auto quadModel = makeTerrainQuadModel(region);
    renderContent.prepareGltfContent(std::move(quadModel), Mat4::identity());
    renderContent.setTerrainRenderContent(true);

    const TileBoundingVolume boundingRegion =
        TileBoundingVolume::fromRegion(region, -25.0, 125.0);

    const bool generated =
        TileRasterOverlayDetailsGenerator::ensureProjectionDetailsFromRegion(
            renderContent,
            boundingRegion,
            RasterOverlayProjection::WebMercator);

    ASSERT_TRUE(generated);
    const GltfModel* model = renderContent.gltfModelForRead();
    ASSERT_NE(nullptr, model);
    ASSERT_EQ(1u, model->primitives.size());
    const GltfPrimitive& primitive =
        model->primitives.front();
    ASSERT_EQ(primitive.vertices.size(), primitive.vertexTexCoords[0].size());
    EXPECT_NEAR(0.0f, primitive.vertexTexCoords[0][0][0], 1e-6f);
    EXPECT_NEAR(0.0f, primitive.vertexTexCoords[0][0][1], 1e-6f);
    EXPECT_NEAR(1.0f, primitive.vertexTexCoords[0][1][0], 1e-6f);
    EXPECT_NEAR(0.0f, primitive.vertexTexCoords[0][1][1], 1e-6f);
    EXPECT_NEAR(1.0f, primitive.vertexTexCoords[0][2][0], 1e-6f);
    EXPECT_NEAR(1.0f, primitive.vertexTexCoords[0][2][1], 1e-6f);
    EXPECT_NEAR(0.0f, primitive.vertexTexCoords[0][3][0], 1e-6f);
    EXPECT_NEAR(1.0f, primitive.vertexTexCoords[0][3][1], 1e-6f);
}

TEST(RasterOverlayDetailsGeneratorTest,
     RegionGenerationWritesGltfOverlayTexCoordsFromRtcWorldPositions) {
    TileRenderContentState renderContent;
    const Rectangle region = Rectangle::fromDegrees(-12.0, -4.0, -6.0, 2.0);
    auto quadModel = makeRtcTerrainQuadModel(region);
    renderContent.prepareGltfContent(std::move(quadModel), Mat4::identity());
    renderContent.setTerrainRenderContent(true);

    const TileBoundingVolume boundingRegion =
        TileBoundingVolume::fromRegion(region, -25.0, 125.0);

    const bool generated =
        TileRasterOverlayDetailsGenerator::ensureProjectionDetailsFromRegion(
            renderContent,
            boundingRegion,
            RasterOverlayProjection::WebMercator);

    ASSERT_TRUE(generated);
    const GltfModel* model = renderContent.gltfModelForRead();
    ASSERT_NE(nullptr, model);
    ASSERT_EQ(1u, model->primitives.size());
    const GltfPrimitive& primitive = model->primitives.front();
    ASSERT_EQ(primitive.vertices.size(), primitive.vertexTexCoords[0].size());
    EXPECT_NEAR(0.0f, primitive.vertexTexCoords[0][0][0], 1e-6f);
    EXPECT_NEAR(0.0f, primitive.vertexTexCoords[0][0][1], 1e-6f);
    EXPECT_NEAR(1.0f, primitive.vertexTexCoords[0][1][0], 1e-6f);
    EXPECT_NEAR(0.0f, primitive.vertexTexCoords[0][1][1], 1e-6f);
    EXPECT_NEAR(1.0f, primitive.vertexTexCoords[0][2][0], 1e-6f);
    EXPECT_NEAR(1.0f, primitive.vertexTexCoords[0][2][1], 1e-6f);
    EXPECT_NEAR(0.0f, primitive.vertexTexCoords[0][3][0], 1e-6f);
    EXPECT_NEAR(1.0f, primitive.vertexTexCoords[0][3][1], 1e-6f);

    const WebMercatorProjection projection(Ellipsoid::WGS84());
    const Rectangle projectedRegion =
        projectRectangleSimple(projection, region);
    const Cartographic localOnlyCartographic =
        Ellipsoid::WGS84().cartesianToCartographic(
            primitive.vertices[2].positionEcef);
    const Vec3 localOnlyProjected =
        projectPosition(projection, localOnlyCartographic);
    const double wrongV =
        (localOnlyProjected.y() - projectedRegion.south()) /
        projectedRegion.height();
    EXPECT_GT(std::abs(wrongV - primitive.vertexTexCoords[0][2][1]), 1e-3);
}

TEST(RasterOverlayDetailsGeneratorTest,
     RegionGenerationUsesLooseRectangleForUvButTightModelBoundingRegion) {
    TileRenderContentState renderContent;
    const Rectangle modelRegion =
        Rectangle::fromDegrees(-12.0, -4.0, -6.0, 2.0);
    const Rectangle looseRegion =
        Rectangle::fromDegrees(-20.0, -10.0, 10.0, 20.0);
    prepareTerrainQuadRenderContent(renderContent, modelRegion);

    const TileBoundingVolume looseBoundingRegion =
        TileBoundingVolume::fromRegion(looseRegion, -25.0, 125.0);

    const bool generated =
        TileRasterOverlayDetailsGenerator::ensureProjectionDetailsFromRegion(
            renderContent,
            looseBoundingRegion,
            RasterOverlayProjection::Geographic);

    ASSERT_TRUE(generated);
    const RasterOverlayDetails& details = renderContent.rasterOverlayDetails();
    ASSERT_EQ(1u, details.rasterOverlayRectangles.size());
    EXPECT_EQ(looseRegion, details.rasterOverlayRectangles[0]);
    expectRectangleNear(modelRegion, details.boundingRegion.rectangle);
    EXPECT_NEAR(0.0, details.boundingRegion.minimumHeight, 1e-6);
    EXPECT_NEAR(0.0, details.boundingRegion.maximumHeight, 1e-6);

    const GltfModel* model = renderContent.gltfModelForRead();
    ASSERT_NE(nullptr, model);
    const GltfPrimitive& primitive = model->primitives.front();
    ASSERT_EQ(primitive.vertices.size(), primitive.vertexTexCoords[0].size());
    EXPECT_NEAR(
        static_cast<float>(
            (modelRegion.west() - looseRegion.west()) /
            looseRegion.width()),
        primitive.vertexTexCoords[0][0][0],
        1e-6f);
    EXPECT_NEAR(
        static_cast<float>(
            (modelRegion.south() - looseRegion.south()) /
            looseRegion.height()),
        primitive.vertexTexCoords[0][0][1],
        1e-6f);
    EXPECT_NEAR(
        static_cast<float>(
            (modelRegion.east() - looseRegion.west()) /
            looseRegion.width()),
        primitive.vertexTexCoords[0][1][0],
        1e-6f);
    EXPECT_NEAR(
        static_cast<float>(
            (modelRegion.north() - looseRegion.south()) /
            looseRegion.height()),
        primitive.vertexTexCoords[0][2][1],
        1e-6f);
}
