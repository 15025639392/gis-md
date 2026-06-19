#include <gtest/gtest.h>

#include "earth_engine/core/math/BoundingCylinderRegion.h"
#include "earth_engine/core/math/MathUtils.h"
#include "earth_engine/core/math/OrientedBoundingBox.h"

#include <glm/gtc/quaternion.hpp>
#include <glm/vec2.hpp>

using namespace earth_engine;

namespace {

void expectVec3Near(const Vec3& actual, const Vec3& expected, double epsilon) {
    EXPECT_NEAR(expected.x(), actual.x(), epsilon);
    EXPECT_NEAR(expected.y(), actual.y(), epsilon);
    EXPECT_NEAR(expected.z(), actual.z(), epsilon);
}

void expectObbNear(const OrientedBoundingBox& actual,
                   const Vec3& center,
                   const Vec3& axis0,
                   const Vec3& axis1,
                   const Vec3& axis2,
                   double epsilon) {
    expectVec3Near(actual.getCenter(), center, epsilon);
    expectVec3Near(actual.getHalfAxis(0), axis0, epsilon);
    expectVec3Near(actual.getHalfAxis(1), axis1, epsilon);
    expectVec3Near(actual.getHalfAxis(2), axis2, epsilon);
}

} // namespace

TEST(BoundingCylinderRegionTest, ConstructorStoresCesiumNativeState) {
    const Vec3 translation(1.0, 2.0, 3.0);
    const glm::dquat rotation(1.0, 0.0, 0.0, 0.0);
    const glm::dvec2 radialBounds(0.5, 1.0);
    const glm::dvec2 angularBounds(-MathUtils::PiOverTwo, 0.0);

    const BoundingCylinderRegion cylinder(
        translation,
        rotation,
        2.0,
        radialBounds,
        angularBounds);

    EXPECT_EQ(translation, cylinder.getTranslation());
    EXPECT_EQ(rotation, cylinder.getRotation());
    EXPECT_DOUBLE_EQ(2.0, cylinder.getHeight());
    EXPECT_EQ(radialBounds, cylinder.getRadialBounds());
    EXPECT_EQ(angularBounds, cylinder.getAngularBounds());
}

TEST(BoundingCylinderRegionTest, WholeCylinderToOrientedBoundingBoxMatchesCesiumNative) {
    const BoundingCylinderRegion region(
        Vec3::zero(),
        glm::dquat(1.0, 0.0, 0.0, 0.0),
        3.0,
        glm::dvec2(0.0, 2.0));

    expectObbNear(region.toOrientedBoundingBox(),
                  Vec3::zero(),
                  Vec3(2.0, 0.0, 0.0),
                  Vec3(0.0, 2.0, 0.0),
                  Vec3(0.0, 0.0, 1.5),
                  0.0);
}

TEST(BoundingCylinderRegionTest, PartialCylinderToOrientedBoundingBoxMatchesCesiumNative) {
    const BoundingCylinderRegion region(
        Vec3::zero(),
        glm::dquat(1.0, 0.0, 0.0, 0.0),
        3.0,
        glm::dvec2(1.0, 2.0),
        glm::dvec2(0.0, MathUtils::PiOverTwo));

    expectObbNear(region.toOrientedBoundingBox(),
                  Vec3(1.0, 1.0, 0.0),
                  Vec3(1.0, 0.0, 0.0),
                  Vec3(0.0, 1.0, 0.0),
                  Vec3(0.0, 0.0, 1.5),
                  0.0);
}

TEST(BoundingCylinderRegionTest,
     ReversedAngularBoundsCrossNegativePiLikeCesiumNative) {
    const BoundingCylinderRegion region(
        Vec3::zero(),
        glm::dquat(1.0, 0.0, 0.0, 0.0),
        3.0,
        glm::dvec2(1.0, 2.0),
        glm::dvec2(MathUtils::PiOverTwo, -MathUtils::PiOverTwo));

    expectObbNear(region.toOrientedBoundingBox(),
                  Vec3(-1.0, 0.0, 0.0),
                  Vec3(1.0, 0.0, 0.0),
                  Vec3(0.0, 2.0, 0.0),
                  Vec3(0.0, 0.0, 1.5),
                  MathUtils::Epsilon6);
}

TEST(BoundingCylinderRegionTest, RotatedTranslatedPartialCylinderMatchesCesiumNative) {
    const BoundingCylinderRegion region(
        Vec3(1.0, 2.0, 3.0),
        glm::dquat(glm::dmat3(
            glm::dvec3(0.0, 1.0, 0.0),
            glm::dvec3(-1.0, 0.0, 0.0),
            glm::dvec3(0.0, 0.0, 1.0))),
        3.0,
        glm::dvec2(1.0, 2.0),
        glm::dvec2(0.0, MathUtils::PiOverTwo));

    expectObbNear(region.toOrientedBoundingBox(),
                  Vec3(0.0, 3.0, 3.0),
                  Vec3(0.0, 1.0, 0.0),
                  Vec3(-1.0, 0.0, 0.0),
                  Vec3(0.0, 0.0, 1.5),
                  MathUtils::Epsilon6);
}
