#include <gtest/gtest.h>

#include "earth_engine/core/math/Mat4.h"
#include "earth_engine/core/geodesy/S2CellBoundingVolume.h"
#include "earth_engine/core/geodesy/S2CellID.h"
#include "earth_engine/core/math/BoundingCylinderRegion.h"
#include "earth_engine/core/math/Rectangle.h"
#include "earth_engine/core/geodesy/Cartographic.h"
#include "earth_engine/core/geodesy/Ellipsoid.h"
#include "earth_engine/core/geodesy/Transforms.h"
#include "earth_engine/tiling/TileBoundingVolume.h"
#include "earth_engine/tiling/TileBoundsMetrics.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <glm/gtc/quaternion.hpp>

using namespace earth_engine;

namespace {

constexpr double kPi = 3.14159265358979323846264338327950288;

void expectRectangleNear(const Rectangle& expected,
                         const Rectangle& actual,
                         double epsilon) {
    EXPECT_NEAR(expected.west(), actual.west(), epsilon);
    EXPECT_NEAR(expected.south(), actual.south(), epsilon);
    EXPECT_NEAR(expected.east(), actual.east(), epsilon);
    EXPECT_NEAR(expected.north(), actual.north(), epsilon);
}

void expectVec3Near(const Vec3& expected,
                    const Vec3& actual,
                    double epsilon) {
    EXPECT_NEAR(expected.x(), actual.x(), epsilon);
    EXPECT_NEAR(expected.y(), actual.y(), epsilon);
    EXPECT_NEAR(expected.z(), actual.z(), epsilon);
}

} // namespace

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

TEST(TileBoundingVolumeTest, TransformCylinderRegionLikeCesiumNative) {
    const TileBoundingVolume volume =
        TileBoundingVolume::fromCylinderRegion(
            BoundingCylinderRegion(
                Vec3::zero(),
                glm::dquat(1.0, 0.0, 0.0, 0.0),
                3.0,
                glm::dvec2(1.0, 2.0),
                glm::dvec2(0.0, kPi * 0.5)));

    const Mat4 transform =
        Mat4::translation(Vec3(10.0, 20.0, 30.0)) *
        Mat4::scale(Vec3(2.0, 3.0, 4.0));
    const TileBoundingVolume transformed = volume.transform(transform);

    EXPECT_EQ(TileBoundingVolumeKind::CylinderRegion, transformed.kind);
    EXPECT_EQ(Vec3(10.0, 20.0, 30.0),
              transformed.cylinderRegion.getTranslation());
    EXPECT_DOUBLE_EQ(12.0, transformed.cylinderRegion.getHeight());
    EXPECT_EQ(glm::dvec2(3.0, 6.0),
              transformed.cylinderRegion.getRadialBounds());
}

TEST(TileBoundingVolumeTest, S2CellStoresHeightsAndIgnoresTransform) {
    const S2CellBoundingVolume s2(
        S2CellID::fromQuadtreeTileID(1, 1, 0, 1),
        10.0,
        20.0);
    const TileBoundingVolume volume = TileBoundingVolume::fromS2Cell(s2);

    const TileBoundingVolume transformed =
        volume.transform(Mat4::translation(Vec3(10.0, 20.0, 30.0)));

    EXPECT_EQ(TileBoundingVolumeKind::S2Cell, transformed.kind);
    EXPECT_EQ(s2.getCellID().getID(), transformed.s2Cell.getCellID().getID());
    EXPECT_DOUBLE_EQ(10.0, transformed.s2Cell.getMinimumHeight());
    EXPECT_DOUBLE_EQ(20.0, transformed.s2Cell.getMaximumHeight());
}

TEST(TileBoundingVolumeTest, S2CellConvertsToBoundingRegionObbLikeCesiumNative) {
    const TileBoundingVolume volume = TileBoundingVolume::fromS2Cell(
        S2CellBoundingVolume(
            S2CellID::fromQuadtreeTileID(1, 1, 0, 1),
            10.0,
            20.0));
    const std::optional<OrientedBoundingBox> expected =
        TileBoundsMetrics::boundingRegionObb(
            volume.s2Cell.getCellID().computeBoundingRectangle(),
            volume.s2Cell.getMinimumHeight(),
            volume.s2Cell.getMaximumHeight());

    const std::optional<OrientedBoundingBox> actual =
        volume.toOrientedBoundingBox();

    ASSERT_TRUE(expected.has_value());
    ASSERT_TRUE(actual.has_value());
    expectVec3Near(expected->getCenter(), actual->getCenter(), 1e-7);
    expectVec3Near(expected->getHalfAxis(0), actual->getHalfAxis(0), 1e-7);
    expectVec3Near(expected->getHalfAxis(1), actual->getHalfAxis(1), 1e-7);
    expectVec3Near(expected->getHalfAxis(2), actual->getHalfAxis(2), 1e-7);
}

TEST(TileBoundingVolumeTest, EstimateGlobeRectangleForS2CellLikeCesiumNative) {
    const S2CellID cellID = S2CellID::fromQuadtreeTileID(1, 1, 0, 1);
    const TileBoundingVolume volume = TileBoundingVolume::fromS2Cell(
        S2CellBoundingVolume(cellID, 10.0, 20.0));

    const std::optional<Rectangle> estimated =
        volume.estimateGlobeRectangle();

    ASSERT_TRUE(estimated.has_value());
    expectRectangleNear(cellID.computeBoundingRectangle(), *estimated, 0.0);
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

    const TileBoundingVolume cylinder =
        TileBoundingVolume::fromCylinderRegion(
            BoundingCylinderRegion(
                Vec3::zero(),
                glm::dquat(1.0, 0.0, 0.0, 0.0),
                3.0,
                glm::dvec2(1.0, 2.0),
                glm::dvec2(0.0, kPi * 0.5)));
    EXPECT_EQ(cylinder.cylinderRegion.getCenter(),
              TileBoundsMetrics::boundingVolumeCenter(cylinder));
}

TEST(TileBoundingVolumeTest, S2CellCenterUsesCellHeightMidpointLikeCesiumNative) {
    const S2CellBoundingVolume s2(
        S2CellID::fromToken("5"),
        1000.0,
        2000.0);
    const TileBoundingVolume volume = TileBoundingVolume::fromS2Cell(s2);
    const Vec3 expectedCenter = Ellipsoid::WGS84().cartographicToCartesian(
        Cartographic::fromRadians(
            s2.getCellID().getCenter().longitude(),
            s2.getCellID().getCenter().latitude(),
            1500.0));

    const Vec3 actualCenter =
        TileBoundsMetrics::boundingVolumeCenter(volume);

    expectVec3Near(expectedCenter, actualCenter, 1e-7);
}

TEST(TileBoundingVolumeTest, S2CellDistanceAndContainsCenterLikeCesiumNative) {
    const S2CellBoundingVolume s2(
        S2CellID::fromToken("1"),
        0.0,
        100000.0);
    const TileBoundingVolume volume = TileBoundingVolume::fromS2Cell(s2);

    EXPECT_DOUBLE_EQ(0.0, s2.computeDistanceSquaredToPosition(s2.getCenter()));
    EXPECT_DOUBLE_EQ(0.0,
                     TileBoundsMetrics::boundingVolumeDistance(
                         volume,
                         s2.getCenter()));
    EXPECT_TRUE(TileBoundsMetrics::boundingVolumeContainsPosition(
        volume,
        s2.getCenter()));
}

TEST(TileBoundingVolumeTest, S2CellDistanceCaseOneMatchesCesiumNative) {
    const S2CellBoundingVolume s2(
        S2CellID::fromToken("1"),
        0.0,
        100000.0);
    constexpr double testDistance = 100.0;
    const std::array<Plane, 6>& planes = s2.getBoundingPlanes();

    const Plane topPlane(
        planes[0].getNormal(),
        planes[0].getDistance() - testDistance);
    Vec3 position = topPlane.projectPointOntoPlane(s2.getCenter());

    EXPECT_NEAR(testDistance,
                std::sqrt(s2.computeDistanceSquaredToPosition(position)),
                1e-7);

    const Plane sidePlane0(
        planes[2].getNormal(),
        planes[2].getDistance() - testDistance);
    const std::array<Vec3, 8>& vertices = s2.getVertices();
    const Vec3 faceCenter =
        ((vertices[0] + vertices[1]) * 0.5 +
         (vertices[4] + vertices[5]) * 0.5) *
        0.5;
    position = sidePlane0.projectPointOntoPlane(faceCenter);

    EXPECT_NEAR(testDistance,
                std::sqrt(s2.computeDistanceSquaredToPosition(position)),
                1e-7);
}

TEST(TileBoundingVolumeTest, S2CellDistanceCaseTwoMatchesCesiumNative) {
    const S2CellBoundingVolume s2(
        S2CellID::fromToken("1"),
        0.0,
        100000.0);
    const std::array<Vec3, 8>& vertices = s2.getVertices();

    Vec3 position = (vertices[0] + vertices[1]) * 0.5;
    position = Vec3(position.x(), position.y(), position.z() - 5.0);
    EXPECT_NEAR(5.0,
                std::sqrt(s2.computeDistanceSquaredToPosition(position)),
                1e-7);

    position = (vertices[0] + vertices[4]) * 0.5;
    position = Vec3(position.x() - 1.0, position.y(), position.z() - 1.0);
    EXPECT_NEAR(2.0,
                s2.computeDistanceSquaredToPosition(position),
                1e-7);

    position = (vertices[5] + vertices[6]) * 0.5;
    position = Vec3(position.x() - 10000.0, position.y() - 1.0, position.z());
    EXPECT_NEAR(10000.0,
                std::sqrt(s2.computeDistanceSquaredToPosition(position)),
                1e-7);
}

TEST(TileBoundingVolumeTest, S2CellDistanceCaseThreeMatchesCesiumNative) {
    const S2CellBoundingVolume s2(
        S2CellID::fromToken("1"),
        0.0,
        100000.0);
    const Vec3 position = s2.getVertices()[2] + Vec3(1.0, 1.0, 1.0);

    EXPECT_NEAR(3.0,
                s2.computeDistanceSquaredToPosition(position),
                1e-7);
}

TEST(TileBoundingVolumeTest, RegionCenterUsesBoundingRegionObbLikeCesiumNative) {
    const TileBoundingVolume region =
        TileBoundingVolume::fromRegion(
            Rectangle::fromDegrees(-10.0, -5.0, 20.0, 15.0),
            100.0,
            2000.0);

    const std::optional<OrientedBoundingBox> regionObb =
        TileBoundsMetrics::boundingRegionObb(
            region.region,
            region.minimumHeight,
            region.maximumHeight);
    ASSERT_TRUE(regionObb.has_value());

    EXPECT_EQ(regionObb->getCenter(),
              TileBoundsMetrics::boundingVolumeCenter(region));
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

TEST(TileBoundingVolumeTest, CylinderRegionConvertsToContainedOrientedBox) {
    const BoundingCylinderRegion cylinder(
        Vec3::zero(),
        glm::dquat(1.0, 0.0, 0.0, 0.0),
        3.0,
        glm::dvec2(1.0, 2.0),
        glm::dvec2(0.0, kPi * 0.5));
    const TileBoundingVolume volume =
        TileBoundingVolume::fromCylinderRegion(cylinder);

    const std::optional<OrientedBoundingBox> box =
        volume.toOrientedBoundingBox();

    ASSERT_TRUE(box.has_value());
    EXPECT_EQ(cylinder.toOrientedBoundingBox().getCenter(), box->getCenter());
    EXPECT_EQ(cylinder.toOrientedBoundingBox().getHalfAxis(0),
              box->getHalfAxis(0));
    EXPECT_EQ(cylinder.toOrientedBoundingBox().getHalfAxis(1),
              box->getHalfAxis(1));
    EXPECT_EQ(cylinder.toOrientedBoundingBox().getHalfAxis(2),
              box->getHalfAxis(2));
}

TEST(TileBoundingVolumeTest, CylinderRegionMetricsUseContainedBox) {
    const BoundingCylinderRegion cylinder(
        Vec3::zero(),
        glm::dquat(1.0, 0.0, 0.0, 0.0),
        4.0,
        glm::dvec2(1.0, 2.0),
        glm::dvec2(0.0, kPi * 0.5));
    const TileBoundingVolume volume =
        TileBoundingVolume::fromCylinderRegion(cylinder);
    const Vec3 outside(5.0, 5.0, 0.0);

    EXPECT_TRUE(TileBoundsMetrics::boundingVolumeContainsPosition(
        volume,
        cylinder.getCenter()));
    EXPECT_DOUBLE_EQ(
        std::sqrt(cylinder.computeDistanceSquaredToPosition(outside)),
        TileBoundsMetrics::boundingVolumeDistance(volume, outside));
}

TEST(TileBoundingVolumeTest, RegionConvertsToBoundingRegionObbLikeCesiumNative) {
    const TileBoundingVolume volume =
        TileBoundingVolume::fromRegion(Rectangle::fromDegrees(-1.0, -2.0, 3.0, 4.0),
                                       10.0,
                                       20.0);
    const std::optional<OrientedBoundingBox> expected =
        TileBoundsMetrics::boundingRegionObb(
            volume.region,
            volume.minimumHeight,
            volume.maximumHeight);
    const std::optional<OrientedBoundingBox> actual =
        volume.toOrientedBoundingBox();

    ASSERT_TRUE(expected.has_value());
    ASSERT_TRUE(actual.has_value());
    EXPECT_EQ(expected->getCenter(), actual->getCenter());
    EXPECT_EQ(expected->getHalfAxis(0), actual->getHalfAxis(0));
    EXPECT_EQ(expected->getHalfAxis(1), actual->getHalfAxis(1));
    EXPECT_EQ(expected->getHalfAxis(2), actual->getHalfAxis(2));
}

TEST(TileBoundingVolumeTest, EstimateGlobeRectangleReturnsRegionRectangle) {
    const Rectangle rectangle = Rectangle::fromDegrees(-10.0, -5.0, 20.0, 15.0);
    const TileBoundingVolume region =
        TileBoundingVolume::fromRegion(rectangle, 100.0, 2000.0);

    const std::optional<Rectangle> estimated =
        region.estimateGlobeRectangle();

    ASSERT_TRUE(estimated.has_value());
    expectRectangleNear(rectangle, *estimated, 0.0);
}

TEST(TileBoundingVolumeTest, EstimateGlobeRectangleReturnsMaximumWhenContainingOrigin) {
    const Rectangle maximum(-kPi, -kPi * 0.5, kPi, kPi * 0.5);
    const TileBoundingVolume sphere =
        TileBoundingVolume::fromSphere(Vec3::zero(), 1.0);
    const TileBoundingVolume box =
        TileBoundingVolume::fromBox(Vec3::zero(),
                                    Vec3(1.0, 0.0, 0.0),
                                    Vec3(0.0, 1.0, 0.0),
                                    Vec3(0.0, 0.0, 1.0));

    ASSERT_TRUE(sphere.estimateGlobeRectangle().has_value());
    ASSERT_TRUE(box.estimateGlobeRectangle().has_value());
    expectRectangleNear(maximum, *sphere.estimateGlobeRectangle(), 0.0);
    expectRectangleNear(maximum, *box.estimateGlobeRectangle(), 0.0);
}

TEST(TileBoundingVolumeTest, EstimateGlobeRectangleForSphereLikeCesiumNative) {
    const Ellipsoid& ellipsoid = Ellipsoid::WGS84();
    const Vec3 center = ellipsoid.cartographicToCartesian(
        Cartographic::fromRadians(0.0, 0.0, 1000.0));
    const TileBoundingVolume sphere =
        TileBoundingVolume::fromSphere(center, 500.0);

    const std::optional<Rectangle> estimated =
        sphere.estimateGlobeRectangle(ellipsoid);

    const Mat4 enuToEcef =
        Transforms::eastNorthUpToFixedFrame(center, ellipsoid);
    const Cartographic east = ellipsoid.cartesianToCartographic(
        enuToEcef * Vec3(500.0, 0.0, 0.0));
    const Cartographic west = ellipsoid.cartesianToCartographic(
        enuToEcef * Vec3(-500.0, 0.0, 0.0));
    const Cartographic north = ellipsoid.cartesianToCartographic(
        enuToEcef * Vec3(0.0, 500.0, 0.0));
    const Cartographic south = ellipsoid.cartesianToCartographic(
        enuToEcef * Vec3(0.0, -500.0, 0.0));
    const Rectangle expected(
        west.longitude(),
        south.latitude(),
        east.longitude(),
        north.latitude());

    ASSERT_TRUE(estimated.has_value());
    expectRectangleNear(expected, *estimated, 1e-14);
}

TEST(TileBoundingVolumeTest, EstimateGlobeRectangleForBoxLikeCesiumNative) {
    const Ellipsoid& ellipsoid = Ellipsoid::WGS84();
    const Vec3 center = ellipsoid.cartographicToCartesian(
        Cartographic::fromDegrees(5.0, 3.0, 1000.0));
    const Mat4 enuToEcef =
        Transforms::eastNorthUpToFixedFrame(center, ellipsoid);
    const Vec3 axis0 = enuToEcef * Vec3(250.0, 0.0, 0.0) - center;
    const Vec3 axis1 = enuToEcef * Vec3(0.0, 400.0, 0.0) - center;
    const Vec3 axis2 = enuToEcef * Vec3(0.0, 0.0, 150.0) - center;
    const TileBoundingVolume box =
        TileBoundingVolume::fromBox(center, axis0, axis1, axis2);

    const std::array<Vec3, 8> corners = {
        center + axis0 + axis1 + axis2,
        center + axis0 + axis1 - axis2,
        center + axis0 - axis1 + axis2,
        center + axis0 - axis1 - axis2,
        center - axis0 + axis1 + axis2,
        center - axis0 + axis1 - axis2,
        center - axis0 - axis1 + axis2,
        center - axis0 - axis1 - axis2};
    double west = kPi;
    double south = kPi * 0.5;
    double east = -kPi;
    double north = -kPi * 0.5;
    for (const Vec3& corner : corners) {
        const Cartographic cartographic =
            ellipsoid.cartesianToCartographic(corner);
        west = std::min(west, cartographic.longitude());
        south = std::min(south, cartographic.latitude());
        east = std::max(east, cartographic.longitude());
        north = std::max(north, cartographic.latitude());
    }

    const std::optional<Rectangle> estimated =
        box.estimateGlobeRectangle(ellipsoid);

    ASSERT_TRUE(estimated.has_value());
    expectRectangleNear(Rectangle(west, south, east, north), *estimated, 1e-14);
}

TEST(TileBoundingVolumeTest, EstimateGlobeRectangleForCylinderUsesContainedBox) {
    const Ellipsoid& ellipsoid = Ellipsoid::WGS84();
    const Vec3 center = ellipsoid.cartographicToCartesian(
        Cartographic::fromDegrees(10.0, 20.0, 1000.0));
    const BoundingCylinderRegion cylinder(
        center,
        glm::dquat(1.0, 0.0, 0.0, 0.0),
        200.0,
        glm::dvec2(50.0, 100.0),
        glm::dvec2(0.0, kPi * 0.5));
    const OrientedBoundingBox box = cylinder.toOrientedBoundingBox();
    const TileBoundingVolume volume =
        TileBoundingVolume::fromCylinderRegion(cylinder);

    const std::optional<Rectangle> estimated =
        volume.estimateGlobeRectangle(ellipsoid);
    const std::optional<Rectangle> expected =
        TileBoundingVolume::fromBox(
            box.getCenter(),
            box.getHalfAxis(0),
            box.getHalfAxis(1),
            box.getHalfAxis(2))
            .estimateGlobeRectangle(ellipsoid);

    ASSERT_TRUE(estimated.has_value());
    ASSERT_TRUE(expected.has_value());
    expectRectangleNear(*expected, *estimated, 1e-14);
}
