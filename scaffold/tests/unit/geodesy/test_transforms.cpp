#include <gtest/gtest.h>

#include "earth_engine/core/geodesy/Transforms.h"
#include "earth_engine/core/math/Mat4.h"
#include "earth_engine/core/math/Vec3.h"

using namespace earth_engine;

namespace {

void expectVectorNear(const Mat4& transform,
                      const Vec3& input,
                      const Vec3& expected) {
    const Vec3 actual = transform.transformVector(input);
    EXPECT_NEAR(expected.x(), actual.x(), 1e-12);
    EXPECT_NEAR(expected.y(), actual.y(), 1e-12);
    EXPECT_NEAR(expected.z(), actual.z(), 1e-12);
}

} // namespace

TEST(TransformsTest, UpAxisTransformsMatchCesiumNativeConstants) {
    // Ported from cesium-native CesiumGeometry/test/TestTransforms.cpp.
    expectVectorNear(Transforms::getUpAxisTransform(UpAxis::Y, UpAxis::Z),
                     Vec3::unitX(),
                     Vec3::unitX());
    expectVectorNear(Transforms::getUpAxisTransform(UpAxis::Y, UpAxis::Z),
                     Vec3::unitY(),
                     Vec3::unitZ());
    expectVectorNear(Transforms::getUpAxisTransform(UpAxis::Y, UpAxis::Z),
                     Vec3::unitZ(),
                     -Vec3::unitY());

    expectVectorNear(Transforms::getUpAxisTransform(UpAxis::X, UpAxis::Z),
                     Vec3::unitX(),
                     Vec3::unitZ());
    expectVectorNear(Transforms::getUpAxisTransform(UpAxis::X, UpAxis::Z),
                     Vec3::unitY(),
                     Vec3::unitY());
    expectVectorNear(Transforms::getUpAxisTransform(UpAxis::X, UpAxis::Z),
                     Vec3::unitZ(),
                     -Vec3::unitX());

    expectVectorNear(Transforms::getUpAxisTransform(UpAxis::X, UpAxis::Y),
                     Vec3::unitX(),
                     Vec3::unitY());
    expectVectorNear(Transforms::getUpAxisTransform(UpAxis::X, UpAxis::Y),
                     Vec3::unitY(),
                     -Vec3::unitX());
    expectVectorNear(Transforms::getUpAxisTransform(UpAxis::X, UpAxis::Y),
                     Vec3::unitZ(),
                     Vec3::unitZ());
}

TEST(TransformsTest, GetUpAxisTransformMatchesCesiumNativeCases) {
    EXPECT_EQ(Mat4::identity(),
              Transforms::getUpAxisTransform(UpAxis::X, UpAxis::X));
    EXPECT_EQ(Mat4::identity(),
              Transforms::getUpAxisTransform(UpAxis::Y, UpAxis::Y));
    EXPECT_EQ(Mat4::identity(),
              Transforms::getUpAxisTransform(UpAxis::Z, UpAxis::Z));

    EXPECT_EQ(Transforms::X_UP_TO_Y_UP(),
              Transforms::getUpAxisTransform(UpAxis::X, UpAxis::Y));
    EXPECT_EQ(Transforms::X_UP_TO_Z_UP(),
              Transforms::getUpAxisTransform(UpAxis::X, UpAxis::Z));
    EXPECT_EQ(Transforms::Y_UP_TO_X_UP(),
              Transforms::getUpAxisTransform(UpAxis::Y, UpAxis::X));
    EXPECT_EQ(Transforms::Y_UP_TO_Z_UP(),
              Transforms::getUpAxisTransform(UpAxis::Y, UpAxis::Z));
    EXPECT_EQ(Transforms::Z_UP_TO_X_UP(),
              Transforms::getUpAxisTransform(UpAxis::Z, UpAxis::X));
    EXPECT_EQ(Transforms::Z_UP_TO_Y_UP(),
              Transforms::getUpAxisTransform(UpAxis::Z, UpAxis::Y));
}
