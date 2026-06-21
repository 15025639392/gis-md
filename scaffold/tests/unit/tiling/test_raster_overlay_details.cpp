#include <gtest/gtest.h>

#include <memory>

#include "earth_engine/core/geodesy/Ellipsoid.h"
#include "earth_engine/core/geodesy/Projection.h"
#include "earth_engine/core/math/Rectangle.h"
#include "earth_engine/tiling/SurfaceTile.h"
#include "earth_engine/tiling/TileBoundingVolume.h"
#include "earth_engine/tiling/TileRasterOverlayDetailsGenerator.h"
#include "earth_engine/tiling/TileRenderContentState.h"

using namespace earth_engine;

TEST(RasterOverlayDetailsTest, MergeAppendsProjectionRectanglesLikeCesiumNative) {
    RasterOverlayDetails first;
    const Rectangle firstRectangle(0.0, 1.0, 2.0, 3.0);
    first.setGeographicRectangle(firstRectangle, -10.0, 20.0);

    RasterOverlayDetails second;
    const Rectangle secondRectangle(4.0, 5.0, 6.0, 7.0);
    second.setGeographicRectangle(secondRectangle, -30.0, 40.0);

    first.merge(second);

    ASSERT_EQ(2u, first.rasterOverlayProjections.size());
    ASSERT_EQ(2u, first.rasterOverlayRectangles.size());
    EXPECT_EQ(RasterOverlayProjection::Geographic,
              first.rasterOverlayProjections[0]);
    EXPECT_EQ(RasterOverlayProjection::Geographic,
              first.rasterOverlayProjections[1]);
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
}

TEST(RasterOverlayDetailsGeneratorTest,
     RegionGenerationAppendsAfterExistingProjectionLikeCesiumNative) {
    TileRenderContentState renderContent;
    auto mesh = std::make_unique<SurfaceTileMesh>();
    mesh->rasterOverlayDetails.rasterOverlayProjections.push_back(
        RasterOverlayProjection::Geographic);
    renderContent.setSurfaceMesh(std::move(mesh));

    const Rectangle region = Rectangle::fromDegrees(-12.0, -4.0, -6.0, 2.0);
    const TileBoundingVolume boundingRegion =
        TileBoundingVolume::fromRegion(region, -25.0, 125.0);

    const bool generated =
        TileRasterOverlayDetailsGenerator::ensureProjectionDetailsFromRegion(
            renderContent,
            boundingRegion,
            RasterOverlayProjection::Geographic);

    EXPECT_TRUE(generated);
    const RasterOverlayDetails& details = renderContent.rasterOverlayDetails();
    ASSERT_EQ(2u, details.rasterOverlayProjections.size());
    ASSERT_EQ(2u, details.rasterOverlayRectangles.size());
    EXPECT_TRUE(details.rasterOverlayRectangles[0].isEmpty());
    EXPECT_EQ(region, details.rasterOverlayRectangles[1]);
    EXPECT_EQ(1, details.textureCoordinateIDForProjection(
                     RasterOverlayProjection::Geographic));
    EXPECT_EQ(region, details.boundingRegion.rectangle);
    EXPECT_DOUBLE_EQ(-25.0, details.boundingRegion.minimumHeight);
    EXPECT_DOUBLE_EQ(125.0, details.boundingRegion.maximumHeight);
}

TEST(RasterOverlayDetailsGeneratorTest,
     RegionGenerationSkipsExistingRectangleLikeCesiumNative) {
    TileRenderContentState renderContent;
    auto mesh = std::make_unique<SurfaceTileMesh>();
    const Rectangle existing = Rectangle::fromDegrees(1.0, 2.0, 3.0, 4.0);
    mesh->rasterOverlayDetails.setGeographicRectangle(existing, 10.0, 20.0);
    renderContent.setSurfaceMesh(std::move(mesh));

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
    renderContent.setSurfaceMesh(std::make_unique<SurfaceTileMesh>());

    const Rectangle region = Rectangle::fromDegrees(-90.0, -45.0, 45.0, 60.0);
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
    EXPECT_EQ(region, details.boundingRegion.rectangle);
    EXPECT_DOUBLE_EQ(-15.0, details.boundingRegion.minimumHeight);
    EXPECT_DOUBLE_EQ(250.0, details.boundingRegion.maximumHeight);
}
