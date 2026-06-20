#include <gtest/gtest.h>

#include "earth_engine/core/math/BoundingCylinderRegion.h"
#include "earth_engine/core/math/MathUtils.h"
#include "earth_engine/core/math/OrientedBoundingBox.h"
#include "earth_engine/core/geodesy/Transforms.h"

#include <glm/gtc/matrix_transform.hpp>
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

TEST(BoundingCylinderRegionTest, TransformSolidCylinderMatchesCesiumNative) {
    glm::dmat4 transform = Transforms::Z_UP_TO_Y_UP().raw();
    transform[3] = glm::dvec4(1.0, 2.0, 3.0, 1.0);

    const BoundingCylinderRegion region(
        Vec3::zero(),
        glm::dquat(1.0, 0.0, 0.0, 0.0),
        3.0,
        glm::dvec2(0.0, 2.0));

    const BoundingCylinderRegion transformedRegion = region.transform(transform);
    EXPECT_EQ(Vec3(1.0, 2.0, 3.0), transformedRegion.getTranslation());

    expectObbNear(transformedRegion.toOrientedBoundingBox(),
                  Vec3(1.0, 2.0, 3.0),
                  Vec3(2.0, 0.0, 0.0),
                  Vec3(0.0, 0.0, -2.0),
                  Vec3(0.0, 1.5, 0.0),
                  MathUtils::Epsilon6);
}

TEST(BoundingCylinderRegionTest, TransformPartialCylinderMatchesCesiumNative) {
    glm::dmat4 transform = Transforms::Z_UP_TO_Y_UP().raw();
    transform[3] = glm::dvec4(1.0, 2.0, 3.0, 1.0);

    const BoundingCylinderRegion region(
        Vec3::zero(),
        glm::dquat(1.0, 0.0, 0.0, 0.0),
        3.0,
        glm::dvec2(1.0, 2.0),
        glm::dvec2(0.0, MathUtils::PiOverTwo));

    const BoundingCylinderRegion transformedRegion = region.transform(transform);
    EXPECT_EQ(Vec3(1.0, 2.0, 3.0), transformedRegion.getTranslation());

    expectObbNear(transformedRegion.toOrientedBoundingBox(),
                  Vec3(2.0, 2.0, 2.0),
                  Vec3(1.0, 0.0, 0.0),
                  Vec3(0.0, 0.0, -1.0),
                  Vec3(0.0, 1.5, 0.0),
                  MathUtils::Epsilon6);
}

TEST(BoundingCylinderRegionTest, TransformPreservesScaledCylinderSemantics) {
    glm::dmat4 transform = Transforms::Z_UP_TO_Y_UP().raw();
    transform[3] = glm::dvec4(1.0, 2.0, 3.0, 1.0);

    glm::dmat4 scale(1.0);
    scale[0][0] = 2.0;
    scale[1][1] = 3.0;
    scale[2][2] = 4.0;

    const BoundingCylinderRegion region(
        Vec3::zero(),
        glm::dquat(1.0, 0.0, 0.0, 0.0),
        3.0,
        glm::dvec2(1.0, 2.0),
        glm::dvec2(0.0, MathUtils::PiOverTwo));

    const BoundingCylinderRegion transformedRegion =
        region.transform(transform * scale);

    EXPECT_DOUBLE_EQ(12.0, transformedRegion.getHeight());
    EXPECT_EQ(glm::dvec2(3.0, 6.0), transformedRegion.getRadialBounds());
}

TEST(BoundingCylinderRegionTest, TransformComposesExistingRotationLikeCesiumNative) {
    glm::dmat4 transform = Transforms::Z_UP_TO_Y_UP().raw();
    transform[3] = glm::dvec4(1.0, 2.0, 3.0, 1.0);

    const Vec3 translation(-1.0, 0.0, 1.0);
    const glm::dquat rotation(Transforms::X_UP_TO_Z_UP().raw());
    const BoundingCylinderRegion region(
        translation,
        rotation,
        3.0,
        glm::dvec2(1.0, 2.0),
        glm::dvec2(0.0, MathUtils::PiOverTwo));

    expectObbNear(region.toOrientedBoundingBox(),
                  Vec3(-1.0, 1.0, 2.0),
                  Vec3(0.0, 0.0, 1.0),
                  Vec3(0.0, 1.0, 0.0),
                  Vec3(-1.5, 0.0, 0.0),
                  MathUtils::Epsilon6);

    const BoundingCylinderRegion transformedRegion = region.transform(transform);
    const glm::dmat4 finalTransform =
        transform *
        glm::translate(glm::dmat4(1.0), translation.raw()) *
        Transforms::X_UP_TO_Z_UP().raw();

    Vec3 expectedTranslation;
    glm::dquat expectedRotation(1.0, 0.0, 0.0, 0.0);
    Transforms::computeTranslationRotationScaleFromMatrix(
        Mat4(finalTransform),
        &expectedTranslation,
        &expectedRotation,
        nullptr);

    EXPECT_EQ(expectedTranslation, transformedRegion.getTranslation());
    for (glm::length_t i = 0; i < 4; ++i) {
        EXPECT_NEAR(expectedRotation[i],
                    transformedRegion.getRotation()[i],
                    MathUtils::Epsilon6);
    }
    expectObbNear(transformedRegion.toOrientedBoundingBox(),
                  Vec3(0.0, 4.0, 2.0),
                  Vec3(0.0, 1.0, 0.0),
                  Vec3(0.0, 0.0, -1.0),
                  Vec3(-1.5, 0.0, 0.0),
                  MathUtils::Epsilon6);
}

TEST(BoundingCylinderRegionTest, QueryMethodsDelegateToOrientedBoundingBoxLikeCesiumNative) {
    const BoundingCylinderRegion region(
        Vec3::zero(),
        glm::dquat(1.0, 0.0, 0.0, 0.0),
        4.0,
        glm::dvec2(0.0, 2.0),
        glm::dvec2(0.0, MathUtils::PiOverTwo));
    const OrientedBoundingBox box = region.toOrientedBoundingBox();

    EXPECT_EQ(box.intersectPlane(Plane(Vec3::unitX(), -2.0)),
              region.intersectPlane(Plane(Vec3::unitX(), -2.0)));
    EXPECT_DOUBLE_EQ(
        box.computeDistanceSquaredToPosition(Vec3(5.0, 0.0, 0.0)),
        region.computeDistanceSquaredToPosition(Vec3(5.0, 0.0, 0.0)));
    EXPECT_EQ(box.contains(Vec3(1.0, 1.0, 0.0)),
              region.contains(Vec3(1.0, 1.0, 0.0)));
    EXPECT_EQ(box.contains(Vec3(-1.0, -1.0, 0.0)),
              region.contains(Vec3(-1.0, -1.0, 0.0)));
}
