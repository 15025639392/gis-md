#include <gtest/gtest.h>

#include "earth_engine/core/geodesy/Cartographic.h"
#include "earth_engine/core/geodesy/Ellipsoid.h"
#include "earth_engine/core/geodesy/EllipsoidTangentPlane.h"
#include "earth_engine/core/geodesy/Transforms.h"
#include "earth_engine/core/math/Mat4.h"
#include "earth_engine/core/math/Vec3.h"

#include <stdexcept>

using namespace earth_engine;

namespace {

void expectVec3Near(const Vec3& actual,
                    const Vec3& expected,
                    double epsilon) {
    EXPECT_NEAR(expected.x(), actual.x(), epsilon);
    EXPECT_NEAR(expected.y(), actual.y(), epsilon);
    EXPECT_NEAR(expected.z(), actual.z(), epsilon);
}

} // namespace

TEST(EllipsoidTangentPlaneTest, ConstructorProjectsOriginToGeodeticSurface) {
    // Source-derived from cesium-native EllipsoidTangentPlane.cpp:
    // the origin constructor first calls ellipsoid.scaleToGeodeticSurface.
    const Ellipsoid& ellipsoid = Ellipsoid::WGS84();
    const Cartographic cartographic =
        Cartographic::fromDegrees(116.397, 39.908, 1200.0);
    const Vec3 input = ellipsoid.cartographicToCartesian(cartographic);
    const Vec3 surface = ellipsoid.scaleToGeodeticSurface(input);

    const EllipsoidTangentPlane tangentPlane(input, ellipsoid);

    expectVec3Near(tangentPlane.getOrigin(), surface, 1e-6);
    expectVec3Near(tangentPlane.getZAxis(),
                   ellipsoid.geodeticSurfaceNormal(surface),
                   1e-12);
    EXPECT_NEAR(0.0,
                tangentPlane.getPlane().getPointDistance(surface),
                1e-9);
}

TEST(EllipsoidTangentPlaneTest, ConstructorRejectsEllipsoidCenter) {
    // Source-derived from cesium-native: center scale returns nullopt and the
    // tangent-plane constructor throws invalid_argument.
    EXPECT_THROW(EllipsoidTangentPlane(Vec3::zero(), Ellipsoid::WGS84()),
                 std::invalid_argument);
}

TEST(EllipsoidTangentPlaneTest, MatrixConstructorStoresFrameColumnsLikeCesiumNative) {
    const Vec3 origin(10.0, 20.0, 30.0);
    const Mat4 frame = Transforms::eastNorthUpToFixedFrame(origin);

    const EllipsoidTangentPlane tangentPlane(frame, Ellipsoid::WGS84());

    expectVec3Near(tangentPlane.getOrigin(), origin, 0.0);
    expectVec3Near(tangentPlane.getXAxis(),
                   Vec3(frame(0, 0), frame(1, 0), frame(2, 0)),
                   0.0);
    expectVec3Near(tangentPlane.getYAxis(),
                   Vec3(frame(0, 1), frame(1, 1), frame(2, 1)),
                   0.0);
    expectVec3Near(tangentPlane.getZAxis(),
                   Vec3(frame(0, 2), frame(1, 2), frame(2, 2)),
                   0.0);
}

TEST(EllipsoidTangentPlaneTest, ProjectPointToNearestOnPlaneUsesLocalAxes) {
    const Ellipsoid& ellipsoid = Ellipsoid::WGS84();
    const Vec3 surface = ellipsoid.cartographicToCartesian(
        Cartographic::fromDegrees(116.397, 39.908, 0.0));
    const EllipsoidTangentPlane tangentPlane(surface, ellipsoid);

    const Vec3 point = tangentPlane.getOrigin() +
                       tangentPlane.getXAxis() * 12.5 +
                       tangentPlane.getYAxis() * -4.25 +
                       tangentPlane.getZAxis() * 100.0;

    const glm::dvec2 projected =
        tangentPlane.projectPointToNearestOnPlane(point);

    EXPECT_NEAR(12.5, projected.x, 1e-9);
    EXPECT_NEAR(-4.25, projected.y, 1e-9);
}

TEST(EllipsoidTangentPlaneTest, ProjectPointBelowPlaneHitsAlongForwardNormal) {
    const Ellipsoid& ellipsoid = Ellipsoid::WGS84();
    const Vec3 surface = ellipsoid.cartographicToCartesian(
        Cartographic::fromDegrees(116.397, 39.908, 0.0));
    const EllipsoidTangentPlane tangentPlane(surface, ellipsoid);

    const Vec3 point = tangentPlane.getOrigin() +
                       tangentPlane.getXAxis() * -7.0 +
                       tangentPlane.getYAxis() * 9.5 -
                       tangentPlane.getZAxis() * 100.0;

    const glm::dvec2 projected =
        tangentPlane.projectPointToNearestOnPlane(point);

    EXPECT_NEAR(-7.0, projected.x, 1e-9);
    EXPECT_NEAR(9.5, projected.y, 1e-9);
}

TEST(EllipsoidTangentPlaneTest, ProjectPointAlreadyOnPlaneReturnsLocalCoordinates) {
    const Ellipsoid& ellipsoid = Ellipsoid::WGS84();
    const Vec3 surface = ellipsoid.cartographicToCartesian(
        Cartographic::fromDegrees(116.397, 39.908, 0.0));
    const EllipsoidTangentPlane tangentPlane(surface, ellipsoid);

    const Vec3 point = tangentPlane.getOrigin() +
                       tangentPlane.getXAxis() * 3.25 +
                       tangentPlane.getYAxis() * 8.0;

    const glm::dvec2 projected =
        tangentPlane.projectPointToNearestOnPlane(point);

    EXPECT_NEAR(3.25, projected.x, 1e-9);
    EXPECT_NEAR(8.0, projected.y, 1e-9);
}
