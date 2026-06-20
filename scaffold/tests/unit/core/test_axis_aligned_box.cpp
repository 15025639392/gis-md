#include <gtest/gtest.h>

#include "earth_engine/core/math/AxisAlignedBox.h"
#include "earth_engine/core/math/Vec3.h"

#include <vector>

using namespace earth_engine;

TEST(AxisAlignedBoxTest, ConstructorMatchesCesiumNativeFields) {
    // Ported from cesium-native CesiumGeometry/test/TestAxisAlignedBox.cpp.
    AxisAlignedBox box(1.0, 2.0, 3.0, 4.0, 5.0, 6.0);

    EXPECT_DOUBLE_EQ(1.0, box.minimumX());
    EXPECT_DOUBLE_EQ(2.0, box.minimumY());
    EXPECT_DOUBLE_EQ(3.0, box.minimumZ());
    EXPECT_DOUBLE_EQ(4.0, box.maximumX());
    EXPECT_DOUBLE_EQ(5.0, box.maximumY());
    EXPECT_DOUBLE_EQ(6.0, box.maximumZ());
    EXPECT_DOUBLE_EQ(3.0, box.lengthX());
    EXPECT_DOUBLE_EQ(3.0, box.lengthY());
    EXPECT_DOUBLE_EQ(3.0, box.lengthZ());
    EXPECT_EQ(Vec3(2.5, 3.5, 4.5), box.center());
}

TEST(AxisAlignedBoxTest, FromPositionsMatchesCesiumNativeCases) {
    AxisAlignedBox empty = AxisAlignedBox::fromPositions({});
    EXPECT_DOUBLE_EQ(0.0, empty.minimumX());
    EXPECT_DOUBLE_EQ(0.0, empty.minimumY());
    EXPECT_DOUBLE_EQ(0.0, empty.minimumZ());
    EXPECT_DOUBLE_EQ(0.0, empty.maximumX());
    EXPECT_DOUBLE_EQ(0.0, empty.maximumY());
    EXPECT_DOUBLE_EQ(0.0, empty.maximumZ());
    EXPECT_DOUBLE_EQ(0.0, empty.lengthX());
    EXPECT_DOUBLE_EQ(0.0, empty.lengthY());
    EXPECT_DOUBLE_EQ(0.0, empty.lengthZ());
    EXPECT_EQ(Vec3::zero(), empty.center());

    const Vec3 position(1.0, 2.0, 3.0);
    AxisAlignedBox single = AxisAlignedBox::fromPositions({position});
    EXPECT_DOUBLE_EQ(position.x(), single.minimumX());
    EXPECT_DOUBLE_EQ(position.y(), single.minimumY());
    EXPECT_DOUBLE_EQ(position.z(), single.minimumZ());
    EXPECT_DOUBLE_EQ(position.x(), single.maximumX());
    EXPECT_DOUBLE_EQ(position.y(), single.maximumY());
    EXPECT_DOUBLE_EQ(position.z(), single.maximumZ());
    EXPECT_EQ(position, single.center());

    const std::vector<Vec3> positions{
        Vec3(1.0, 2.0, 3.0),
        Vec3(-2.0, 0.4, -10.0),
        Vec3(0.1, 4.3, 11.0),
        Vec3(0.5, 0.5, 2.7)
    };
    AxisAlignedBox multiple = AxisAlignedBox::fromPositions(positions);
    EXPECT_DOUBLE_EQ(-2.0, multiple.minimumX());
    EXPECT_DOUBLE_EQ(0.4, multiple.minimumY());
    EXPECT_DOUBLE_EQ(-10.0, multiple.minimumZ());
    EXPECT_DOUBLE_EQ(1.0, multiple.maximumX());
    EXPECT_DOUBLE_EQ(4.3, multiple.maximumY());
    EXPECT_DOUBLE_EQ(11.0, multiple.maximumZ());
    EXPECT_DOUBLE_EQ(3.0, multiple.lengthX());
    EXPECT_DOUBLE_EQ(3.9, multiple.lengthY());
    EXPECT_DOUBLE_EQ(21.0, multiple.lengthZ());
    EXPECT_EQ(Vec3(-0.5, 2.35, 0.5), multiple.center());
}

TEST(AxisAlignedBoxTest, ContainsIncludesBoundaryLikeCesiumNative) {
    AxisAlignedBox box(1.0, 2.0, 3.0, 4.0, 5.0, 6.0);

    EXPECT_TRUE(box.contains(Vec3(1.0, 2.0, 3.0)));
    EXPECT_TRUE(box.contains(Vec3(4.0, 5.0, 6.0)));
    EXPECT_TRUE(box.contains(Vec3(2.5, 3.5, 4.5)));
    EXPECT_FALSE(box.contains(Vec3(0.999, 3.5, 4.5)));
    EXPECT_FALSE(box.contains(Vec3(2.5, 5.001, 4.5)));
    EXPECT_FALSE(box.contains(Vec3(2.5, 3.5, 6.001)));
}
