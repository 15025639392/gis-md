#include <gtest/gtest.h>

#include "earth_engine/core/math/Mat4.h"
#include "earth_engine/core/math/Rectangle.h"
#include "earth_engine/tiling/TileBoundingVolume.h"
#include "earth_engine/tiling/TileBoundsMetrics.h"

using namespace earth_engine;

TEST(TileBoundingVolumeTest, TransformLeavesRegionUnchangedLikeCesiumNative) {
    const Rectangle region = Rectangle::fromDegrees(-1.0, -2.0, 3.0, 4.0);
    const TileBoundingVolume volume =
        TileBoundingVolume::fromRegion(region, 10.0, 20.0);

    const TileBoundingVolume transformed =
        volume.transform(Mat4::translation(Vec3(100.0, 200.0, 300.0)));

    EXPECT_EQ(TileBoundingVolumeKind::Region, transformed.kind);
    EXPECT_DOUBLE_EQ(region.west(), transformed.region.west());
    EXPECT_DOUBLE_EQ(region.south(), transformed.region.south());
    EXPECT_DOUBLE_EQ(region.east(), transformed.region.east());
    EXPECT_DOUBLE_EQ(region.north(), transformed.region.north());
    EXPECT_DOUBLE_EQ(10.0, transformed.minimumHeight);
    EXPECT_DOUBLE_EQ(20.0, transformed.maximumHeight);
}

TEST(TileBoundingVolumeTest, TransformSphereLikeCesiumNative) {
    const TileBoundingVolume volume =
        TileBoundingVolume::fromSphere(Vec3(1.0, 2.0, 3.0), 5.0);

    const TileBoundingVolume transformed = volume.transform(
        Mat4::translation(Vec3(10.0, 20.0, 30.0)) *
        Mat4::scale(Vec3(2.0, 3.0, 4.0)));

    EXPECT_EQ(TileBoundingVolumeKind::Sphere, transformed.kind);
    EXPECT_DOUBLE_EQ(12.0, transformed.sphere.getCenter().x());
    EXPECT_DOUBLE_EQ(26.0, transformed.sphere.getCenter().y());
    EXPECT_DOUBLE_EQ(42.0, transformed.sphere.getCenter().z());
    EXPECT_DOUBLE_EQ(20.0, transformed.sphere.getRadius());
}

TEST(TileBoundingVolumeTest, TransformBoxLikeCesiumNative) {
    const TileBoundingVolume volume =
        TileBoundingVolume::fromBox(Vec3(1.0, 2.0, 3.0),
                                    Vec3(2.0, 0.0, 0.0),
                                    Vec3(0.0, 3.0, 0.0),
                                    Vec3(0.0, 0.0, 4.0));

    const TileBoundingVolume transformed = volume.transform(
        Mat4::translation(Vec3(10.0, 20.0, 30.0)) *
        Mat4::scale(Vec3(2.0, 3.0, 4.0)));

    EXPECT_EQ(TileBoundingVolumeKind::Box, transformed.kind);
    EXPECT_DOUBLE_EQ(12.0, transformed.box.getCenter().x());
    EXPECT_DOUBLE_EQ(26.0, transformed.box.getCenter().y());
    EXPECT_DOUBLE_EQ(42.0, transformed.box.getCenter().z());
    EXPECT_DOUBLE_EQ(4.0, transformed.box.getHalfAxis(0).x());
    EXPECT_DOUBLE_EQ(9.0, transformed.box.getHalfAxis(1).y());
    EXPECT_DOUBLE_EQ(16.0, transformed.box.getHalfAxis(2).z());
}

TEST(TileBoundingVolumeTest, CenterUsesContainedVolumeKind) {
    const TileBoundingVolume sphere =
        TileBoundingVolume::fromSphere(Vec3(1.0, 2.0, 3.0), 5.0);
    const TileBoundingVolume box =
        TileBoundingVolume::fromBox(Vec3(4.0, 5.0, 6.0),
                                    Vec3(1.0, 0.0, 0.0),
                                    Vec3(0.0, 1.0, 0.0),
                                    Vec3(0.0, 0.0, 1.0));

    EXPECT_EQ(Vec3(1.0, 2.0, 3.0),
              TileBoundsMetrics::boundingVolumeCenter(sphere));
    EXPECT_EQ(Vec3(4.0, 5.0, 6.0),
              TileBoundsMetrics::boundingVolumeCenter(box));
}

TEST(TileBoundingVolumeTest, SphereConvertsToCircumscribedOrientedBox) {
    const TileBoundingVolume sphere =
        TileBoundingVolume::fromSphere(Vec3(1.0, 2.0, 3.0), 10.0);

    const std::optional<OrientedBoundingBox> box =
        sphere.toOrientedBoundingBox();

    ASSERT_TRUE(box.has_value());
    EXPECT_EQ(Vec3(1.0, 2.0, 3.0), box->getCenter());
    EXPECT_EQ(Vec3(20.0, 20.0, 20.0), box->getLengths());
}

TEST(TileBoundingVolumeTest, BoxConvertsToContainedOrientedBox) {
    const TileBoundingVolume volume =
        TileBoundingVolume::fromBox(Vec3(1.0, 2.0, 3.0),
                                    Vec3(2.0, 0.0, 0.0),
                                    Vec3(0.0, 3.0, 0.0),
                                    Vec3(0.0, 0.0, 4.0));

    const std::optional<OrientedBoundingBox> box =
        volume.toOrientedBoundingBox();

    ASSERT_TRUE(box.has_value());
    EXPECT_EQ(volume.box.getCenter(), box->getCenter());
    EXPECT_EQ(volume.box.getHalfAxis(0), box->getHalfAxis(0));
    EXPECT_EQ(volume.box.getHalfAxis(1), box->getHalfAxis(1));
    EXPECT_EQ(volume.box.getHalfAxis(2), box->getHalfAxis(2));
}

TEST(TileBoundingVolumeTest, RegionHasNoLocalOrientedBoxConversion) {
    const TileBoundingVolume volume =
        TileBoundingVolume::fromRegion(Rectangle::fromDegrees(-1.0, -2.0, 3.0, 4.0),
                                       10.0,
                                       20.0);

    EXPECT_FALSE(volume.toOrientedBoundingBox().has_value());
}
