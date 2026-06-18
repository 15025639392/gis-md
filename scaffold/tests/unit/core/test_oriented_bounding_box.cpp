#include <gtest/gtest.h>
#include <cmath>
#include "earth_engine/core/math/OrientedBoundingBox.h"
#include "earth_engine/core/math/Plane.h"
#include "earth_engine/core/math/Vec3.h"

using namespace earth_engine;

namespace {
OrientedBoundingBox unitBox() {
    return OrientedBoundingBox(Vec3::zero(),
                               Vec3(0.5, 0.0, 0.0),
                               Vec3(0.0, 0.5, 0.0),
                               Vec3(0.0, 0.0, 0.5));
}
} // namespace

TEST(OrientedBoundingBoxTest, IntersectPlaneMatchesCesiumNativeFaceCases) {
    // Condensed from cesium-native
    // CesiumGeometry/test/TestOrientedBoundingBox.cpp face-plane cases.
    OrientedBoundingBox box = unitBox();

    EXPECT_EQ(1, box.intersectPlane(Plane(Vec3(1.0, 0.0, 0.0), 0.50001)));
    EXPECT_EQ(0, box.intersectPlane(Plane(Vec3(1.0, 0.0, 0.0), -0.49999)));
    EXPECT_EQ(-1, box.intersectPlane(Plane(Vec3(1.0, 0.0, 0.0), -0.50001)));

    EXPECT_EQ(1, box.intersectPlane(Plane(Vec3(0.0, -1.0, 0.0), 0.50001)));
    EXPECT_EQ(0, box.intersectPlane(Plane(Vec3(0.0, -1.0, 0.0), -0.49999)));
    EXPECT_EQ(-1, box.intersectPlane(Plane(Vec3(0.0, -1.0, 0.0), -0.50001)));
}

TEST(OrientedBoundingBoxTest, IntersectPlaneMatchesCesiumNativeEdgeCase) {
    OrientedBoundingBox box = unitBox();
    const double edgeDistance = std::sqrt(0.5);
    Vec3 normal = Vec3(1.0, 1.0, 0.0).normalized();

    EXPECT_EQ(1, box.intersectPlane(Plane(normal, edgeDistance + 0.00001)));
    EXPECT_EQ(0, box.intersectPlane(Plane(normal, edgeDistance - 0.00001)));
    EXPECT_EQ(0, box.intersectPlane(Plane(normal, -edgeDistance + 0.00001)));
    EXPECT_EQ(-1, box.intersectPlane(Plane(normal, -edgeDistance - 0.00001)));
}

TEST(OrientedBoundingBoxTest, IntersectPlaneMatchesCesiumNativeCornerCase) {
    OrientedBoundingBox box = unitBox();
    const double cornerDistance = std::sqrt(3.0 / 4.0);
    Vec3 normal = Vec3(1.0, 1.0, 1.0).normalized();

    EXPECT_EQ(1, box.intersectPlane(Plane(normal, cornerDistance + 0.00001)));
    EXPECT_EQ(0, box.intersectPlane(Plane(normal, cornerDistance - 0.00001)));
    EXPECT_EQ(0, box.intersectPlane(Plane(normal, -cornerDistance + 0.00001)));
    EXPECT_EQ(-1, box.intersectPlane(Plane(normal, -cornerDistance - 0.00001)));
}

TEST(OrientedBoundingBoxTest, DistanceSquaredToPositionClampsToBox) {
    OrientedBoundingBox box = unitBox();

    EXPECT_DOUBLE_EQ(0.0, box.computeDistanceSquaredToPosition(Vec3::zero()));
    EXPECT_DOUBLE_EQ(0.0,
                     box.computeDistanceSquaredToPosition(Vec3(0.5, 0.0, 0.0)));
    EXPECT_NEAR(2.25,
                box.computeDistanceSquaredToPosition(Vec3(2.0, 0.0, 0.0)),
                1e-14);
    EXPECT_NEAR(0.75,
                box.computeDistanceSquaredToPosition(Vec3(1.0, 1.0, 1.0)),
                1e-14);
}
