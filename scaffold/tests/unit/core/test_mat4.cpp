#include <gtest/gtest.h>

#include "earth_engine/core/math/Mat4.h"
#include "earth_engine/core/math/Vec3.h"

#include <glm/glm.hpp>

using namespace earth_engine;

namespace {

void expectVec3Near(const Vec3& actual, const Vec3& expected, double epsilon) {
    EXPECT_NEAR(expected.x(), actual.x(), epsilon);
    EXPECT_NEAR(expected.y(), actual.y(), epsilon);
    EXPECT_NEAR(expected.z(), actual.z(), epsilon);
}

} // namespace

TEST(Mat4Test, RowColumnAccessorMatchesGlmColumnMajorStorage) {
    const Mat4 matrix(glm::dmat4(
        glm::dvec4(1.0, 2.0, 3.0, 4.0),
        glm::dvec4(5.0, 6.0, 7.0, 8.0),
        glm::dvec4(9.0, 10.0, 11.0, 12.0),
        glm::dvec4(13.0, 14.0, 15.0, 16.0)));

    EXPECT_DOUBLE_EQ(1.0, matrix(0, 0));
    EXPECT_DOUBLE_EQ(2.0, matrix(1, 0));
    EXPECT_DOUBLE_EQ(5.0, matrix(0, 1));
    EXPECT_DOUBLE_EQ(16.0, matrix(3, 3));
}

TEST(Mat4Test, TransformPointUsesCesiumNativeWOneSemantics) {
    const Mat4 transform =
        Mat4::translation(Vec3(10.0, 20.0, 30.0)) *
        Mat4::scale(Vec3(2.0, 3.0, 4.0));

    expectVec3Near(transform.transformPoint(Vec3(1.0, 2.0, 3.0)),
                   Vec3(12.0, 26.0, 42.0),
                   1e-12);
    expectVec3Near(transform * Vec3(1.0, 2.0, 3.0),
                   Vec3(12.0, 26.0, 42.0),
                   1e-12);
}

TEST(Mat4Test, TransformPointAppliesHomogeneousPerspectiveDivide) {
    // Source-derived from cesium-native's GLM matrix/vector semantics:
    // points are transformed with w=1 and converted back from homogeneous
    // coordinates, so non-affine matrices must divide xyz by the resulting w.
    glm::dmat4 raw(1.0);
    raw[0][0] = 4.0;
    raw[1][1] = 6.0;
    raw[2][2] = 8.0;
    raw[3][3] = 2.0;
    const Mat4 transform(raw);

    expectVec3Near(transform.transformPoint(Vec3(1.0, 2.0, 3.0)),
                   Vec3(2.0, 6.0, 12.0),
                   1e-12);
}

TEST(Mat4Test, TransformVectorUsesCesiumNativeWZeroSemantics) {
    const Mat4 transform =
        Mat4::translation(Vec3(10.0, 20.0, 30.0)) *
        Mat4::rotationZ(1.57079632679489661923) *
        Mat4::scale(Vec3(2.0, 3.0, 4.0));

    expectVec3Near(transform.transformVector(Vec3::unitX()),
                   Vec3(0.0, 2.0, 0.0),
                   1e-12);
    expectVec3Near(transform.transformVector(Vec3::unitY()),
                   Vec3(-3.0, 0.0, 0.0),
                   1e-12);
    expectVec3Near(transform.transformVector(Vec3::unitZ()),
                   Vec3(0.0, 0.0, 4.0),
                   1e-12);
}

TEST(Mat4Test, InverseRoundtripsAffinePoint) {
    const Mat4 transform =
        Mat4::translation(Vec3(10.0, -20.0, 30.0)) *
        Mat4::rotationX(0.25) *
        Mat4::scale(Vec3(2.0, 3.0, 4.0));
    const Vec3 point(5.0, 6.0, 7.0);

    expectVec3Near(transform.inverse().transformPoint(transform.transformPoint(point)),
                   point,
                   1e-12);
}
