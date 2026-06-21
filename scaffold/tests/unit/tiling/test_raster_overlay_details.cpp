#include <gtest/gtest.h>

#include "earth_engine/core/math/Rectangle.h"
#include "earth_engine/tiling/SurfaceTile.h"

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
