#include <gtest/gtest.h>

#include "earth_engine/core/geodesy/Ellipsoid.h"
#include "earth_engine/core/geodesy/S2CellBoundingVolume.h"
#include "earth_engine/core/math/MathUtils.h"
#include "earth_engine/core/math/Plane.h"

#include <cmath>

using namespace earth_engine;

TEST(S2CellBoundingVolumeTest, DistanceIsZeroInsideBoundingVolumeLikeCesiumNative) {
    const S2CellBoundingVolume volume(
        S2CellID::fromToken("1"),
        0.0,
        100000.0);

    EXPECT_DOUBLE_EQ(
        0.0,
        volume.computeDistanceSquaredToPosition(volume.getCenter()));
}

TEST(S2CellBoundingVolumeTest, DistanceToOneSelectedPlaneMatchesCesiumNative) {
    const S2CellBoundingVolume volume(
        S2CellID::fromToken("1"),
        0.0,
        100000.0);
    constexpr double testDistance = 100.0;
    const auto& planes = volume.getBoundingPlanes();

    const Plane topPlane(
        planes[0].getNormal(),
        planes[0].getDistance() - testDistance);
    Vec3 position = topPlane.projectPointOntoPlane(volume.getCenter());
    EXPECT_TRUE(MathUtils::equalsEpsilon(
        std::sqrt(volume.computeDistanceSquaredToPosition(position)),
        testDistance,
        0.0,
        MathUtils::Epsilon7));

    const Plane sidePlane(
        planes[2].getNormal(),
        planes[2].getDistance() - testDistance);
    const auto& vertices = volume.getVertices();
    const Vec3 faceCenter =
        ((vertices[0] + vertices[1]) * 0.5 +
         (vertices[4] + vertices[5]) * 0.5) *
        0.5;
    position = sidePlane.projectPointOntoPlane(faceCenter);
    EXPECT_TRUE(MathUtils::equalsEpsilon(
        std::sqrt(volume.computeDistanceSquaredToPosition(position)),
        testDistance,
        0.0,
        MathUtils::Epsilon7));
}

TEST(S2CellBoundingVolumeTest, DistanceToTwoSelectedPlanesMatchesCesiumNative) {
    const S2CellBoundingVolume volume(
        S2CellID::fromToken("1"),
        0.0,
        100000.0);
    const auto& vertices = volume.getVertices();

    constexpr double topSideDistance = 5.0;
    Vec3 position = (vertices[0] + vertices[1]) * 0.5;
    position.z() -= topSideDistance;
    EXPECT_TRUE(MathUtils::equalsEpsilon(
        std::sqrt(volume.computeDistanceSquaredToPosition(position)),
        topSideDistance,
        0.0,
        MathUtils::Epsilon7));

    position = (vertices[0] + vertices[4]) * 0.5;
    position.x() -= 1.0;
    position.z() -= 1.0;
    EXPECT_TRUE(MathUtils::equalsEpsilon(
        volume.computeDistanceSquaredToPosition(position),
        2.0,
        0.0,
        MathUtils::Epsilon7));

    position = (vertices[5] + vertices[6]) * 0.5;
    position.x() -= 10000.0;
    position.y() -= 1.0;
    EXPECT_TRUE(MathUtils::equalsEpsilon(
        std::sqrt(volume.computeDistanceSquaredToPosition(position)),
        10000.0,
        0.0,
        MathUtils::Epsilon7));
}

TEST(S2CellBoundingVolumeTest, DistanceToThreeSelectedPlanesMatchesCesiumNative) {
    const S2CellBoundingVolume volume(
        S2CellID::fromToken("1"),
        0.0,
        100000.0);

    const Vec3 position = volume.getVertices()[2] + Vec3(1.0, 1.0, 1.0);
    EXPECT_TRUE(MathUtils::equalsEpsilon(
        volume.computeDistanceSquaredToPosition(position),
        3.0,
        0.0,
        MathUtils::Epsilon7));
}

TEST(S2CellBoundingVolumeTest, DistanceToMoreThanThreeSelectedPlanesMatchesCesiumNative) {
    const S2CellBoundingVolume volume(
        S2CellID::fromToken("1"),
        0.0,
        100000.0);

    const Vec3 position(-Ellipsoid::WGS84().maximumRadius(), 0.0, 0.0);
    const double expectedDistance =
        Ellipsoid::WGS84().maximumRadius() +
        volume.getBoundingPlanes()[1].getDistance();
    EXPECT_TRUE(MathUtils::equalsEpsilon(
        std::sqrt(volume.computeDistanceSquaredToPosition(position)),
        expectedDistance,
        0.0,
        MathUtils::Epsilon7));
}

TEST(S2CellBoundingVolumeTest, IntersectPlaneMatchesCesiumNative) {
    const S2CellBoundingVolume volume(
        S2CellID::fromToken("1"),
        0.0,
        100000.0);

    EXPECT_EQ(0, volume.intersectPlane(Plane::ORIGIN_ZX));

    const Plane outsidePlane(
        Plane::ORIGIN_YZ.getNormal(),
        Plane::ORIGIN_YZ.getDistance() -
            2.0 * Ellipsoid::WGS84().maximumRadius());
    EXPECT_EQ(-1, volume.intersectPlane(outsidePlane));

    EXPECT_EQ(1, volume.intersectPlane(Plane::ORIGIN_YZ));
}

TEST(S2CellBoundingVolumeTest, CanConstructNorthPoleFaceLikeCesiumNative) {
    const S2CellBoundingVolume face2Root(
        S2CellID::fromToken("5"),
        1000.0,
        2000.0);

    EXPECT_TRUE(face2Root.getCellID().isValid());
    EXPECT_EQ(5764607523034234880ULL, face2Root.getCellID().getID());
}
