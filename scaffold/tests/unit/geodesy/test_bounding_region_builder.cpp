#include "earth_engine/core/geodesy/BoundingRegionBuilder.h"

#include "earth_engine/core/math/MathUtils.h"

#include <gtest/gtest.h>

using earth_engine::BoundingRegionBuilder;
using earth_engine::Cartographic;
using earth_engine::MathUtils;
using earth_engine::Rectangle;

namespace {

void expectRectangleNear(const Rectangle& actual,
                         const Rectangle& expected,
                         double epsilon = MathUtils::Epsilon15) {
    EXPECT_NEAR(actual.west(), expected.west(), epsilon);
    EXPECT_NEAR(actual.south(), expected.south(), epsilon);
    EXPECT_NEAR(actual.east(), expected.east(), epsilon);
    EXPECT_NEAR(actual.north(), expected.north(), epsilon);
}

} // namespace

TEST(BoundingRegionBuilderTest, EmptyBuilderReturnsEmptyRegionLikeCesiumNative) {
    BoundingRegionBuilder builder;

    const auto region = builder.toRegion();
    EXPECT_EQ(Rectangle::EMPTY, region.rectangle);
    EXPECT_DOUBLE_EQ(1.0, region.minimumHeight);
    EXPECT_DOUBLE_EQ(-1.0, region.maximumHeight);
    EXPECT_EQ(Rectangle::EMPTY, builder.toRectangle());
}

TEST(BoundingRegionBuilderTest, ExpandsPositionsAcrossShortestLongitudeArc) {
    BoundingRegionBuilder builder;
    EXPECT_TRUE(builder.expandToIncludePosition(Cartographic(0.0, 0.0, 10.0)));
    EXPECT_TRUE(builder.expandToIncludePosition(
        Cartographic(MathUtils::OnePi, 1.0, 20.0)));

    const Rectangle base = builder.toRectangle();
    EXPECT_TRUE(base.contains(0.0, 0.0));
    EXPECT_TRUE(base.contains(1.0, 0.0));
    EXPECT_FALSE(base.contains(-1.0, 0.0));
    EXPECT_TRUE(base.contains(0.0, 1.0));
    EXPECT_FALSE(base.contains(0.0, -1.0));

    BoundingRegionBuilder simpleBuilder = builder;
    EXPECT_TRUE(simpleBuilder.expandToIncludePosition(Cartographic(-1.0, 1.0)));
    const Rectangle simple = simpleBuilder.toRectangle();
    EXPECT_TRUE(simple.contains(0.0, 0.0));
    EXPECT_TRUE(simple.contains(1.0, 0.0));
    EXPECT_TRUE(simple.contains(-1.0, 0.0));
    EXPECT_FALSE(simple.contains(-3.0, 0.0));

    BoundingRegionBuilder wrappedBuilder = builder;
    EXPECT_TRUE(wrappedBuilder.expandToIncludePosition(Cartographic(-3.0, 1.0)));
    const Rectangle wrapped = wrappedBuilder.toRectangle();
    EXPECT_TRUE(wrapped.contains(0.0, 0.0));
    EXPECT_TRUE(wrapped.contains(1.0, 0.0));
    EXPECT_FALSE(wrapped.contains(-1.0, 0.0));
    EXPECT_TRUE(wrapped.contains(-3.0, 0.0));

    const auto region = builder.toRegion();
    EXPECT_DOUBLE_EQ(10.0, region.minimumHeight);
    EXPECT_DOUBLE_EQ(20.0, region.maximumHeight);
}

TEST(BoundingRegionBuilderTest, PolePositionsExpandLatitudeAndHeightOnly) {
    BoundingRegionBuilder builder;

    EXPECT_TRUE(builder.expandToIncludePosition(
        Cartographic(1.0, MathUtils::PiOverTwo, -5.0)));

    const auto region = builder.toRegion();
    EXPECT_EQ(Rectangle::EMPTY, region.rectangle);
    EXPECT_DOUBLE_EQ(1.0, region.minimumHeight);
    EXPECT_DOUBLE_EQ(-1.0, region.maximumHeight);

    EXPECT_TRUE(builder.expandToIncludePosition(Cartographic(0.25, 0.25, 8.0)));
    const auto expanded = builder.toRegion();
    EXPECT_TRUE(expanded.rectangle.contains(0.25, 0.25));
    EXPECT_TRUE(expanded.rectangle.contains(0.25, MathUtils::PiOverTwo));
    EXPECT_FALSE(expanded.rectangle.contains(1.0, 0.25));
    EXPECT_DOUBLE_EQ(-5.0, expanded.minimumHeight);
    EXPECT_DOUBLE_EQ(8.0, expanded.maximumHeight);
}

TEST(BoundingRegionBuilderTest, ExpandsRectanglesWithAntimeridianSemantics) {
    BoundingRegionBuilder builder;
    EXPECT_TRUE(builder.expandToIncludeRectangle(
        Rectangle::fromDegrees(170.0, -10.0, 175.0, 20.0)));
    EXPECT_TRUE(builder.expandToIncludeRectangle(
        Rectangle::fromDegrees(176.0, -10.0, -175.0, 20.0)));

    expectRectangleNear(
        builder.toRectangle(),
        Rectangle::fromDegrees(170.0, -10.0, -175.0, 20.0));

    EXPECT_FALSE(builder.expandToIncludeRectangle(
        Rectangle::fromDegrees(171.0, -9.0, -176.0, 19.0)));
}

TEST(BoundingRegionBuilderTest, ExpandsBoundingRegionRectangleAndHeights) {
    BoundingRegionBuilder builder;
    BoundingRegionBuilder::BoundingRegion region{
        Rectangle(0.1, 0.2, 0.3, 0.4),
        -1.0,
        2.0};

    EXPECT_TRUE(builder.expandToIncludeRegion(region));
    auto result = builder.toRegion();
    expectRectangleNear(result.rectangle, region.rectangle);
    EXPECT_DOUBLE_EQ(-1.0, result.minimumHeight);
    EXPECT_DOUBLE_EQ(2.0, result.maximumHeight);

    EXPECT_FALSE(builder.expandToIncludeRegion(
        {Rectangle(0.15, 0.25, 0.25, 0.35), -0.5, 1.5}));

    EXPECT_TRUE(builder.expandToIncludeRegion(
        {Rectangle(0.05, 0.15, 0.35, 0.45), -1.5, 2.5}));
    result = builder.toRegion();
    expectRectangleNear(result.rectangle, Rectangle(0.05, 0.15, 0.35, 0.45));
    EXPECT_DOUBLE_EQ(-1.5, result.minimumHeight);
    EXPECT_DOUBLE_EQ(2.5, result.maximumHeight);
}
