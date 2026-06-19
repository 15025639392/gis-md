#include <gtest/gtest.h>

#include "earth_engine/core/geodesy/Transforms.h"
#include "earth_engine/core/math/MathUtils.h"
#include "earth_engine/core/math/Mat4.h"
#include "earth_engine/core/math/Vec3.h"

#include <cmath>
#include <glm/gtc/quaternion.hpp>
#include <limits>

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

void expectVec3Near(const Vec3& actual,
                    const Vec3& expected,
                    double epsilon) {
    EXPECT_NEAR(expected.x(), actual.x(), epsilon);
    EXPECT_NEAR(expected.y(), actual.y(), epsilon);
    EXPECT_NEAR(expected.z(), actual.z(), epsilon);
}

void expectMatrixNear(const Mat4& actual,
                      const Mat4& expected,
                      double epsilon) {
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            EXPECT_NEAR(expected(row, col), actual(row, col), epsilon)
                << "row " << row << " col " << col;
        }
    }
}

bool pointInClipVolume(const glm::dvec4& point) {
    const double w = point.w;
    return -w <= point.x && point.x <= w &&
           -w <= point.y && point.y <= w &&
           0.0 <= point.z && point.z <= w;
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
    expectVectorNear(Transforms::getUpAxisTransform(UpAxis::Z, UpAxis::Y),
                     Vec3::unitX(),
                     Vec3::unitX());
    expectVectorNear(Transforms::getUpAxisTransform(UpAxis::Z, UpAxis::Y),
                     Vec3::unitY(),
                     -Vec3::unitZ());
    expectVectorNear(Transforms::getUpAxisTransform(UpAxis::Z, UpAxis::Y),
                     Vec3::unitZ(),
                     Vec3::unitY());

    expectVectorNear(Transforms::getUpAxisTransform(UpAxis::X, UpAxis::Z),
                     Vec3::unitX(),
                     Vec3::unitZ());
    expectVectorNear(Transforms::getUpAxisTransform(UpAxis::X, UpAxis::Z),
                     Vec3::unitY(),
                     Vec3::unitY());
    expectVectorNear(Transforms::getUpAxisTransform(UpAxis::X, UpAxis::Z),
                     Vec3::unitZ(),
                     -Vec3::unitX());
    expectVectorNear(Transforms::getUpAxisTransform(UpAxis::Z, UpAxis::X),
                     Vec3::unitX(),
                     -Vec3::unitZ());
    expectVectorNear(Transforms::getUpAxisTransform(UpAxis::Z, UpAxis::X),
                     Vec3::unitY(),
                     Vec3::unitY());
    expectVectorNear(Transforms::getUpAxisTransform(UpAxis::Z, UpAxis::X),
                     Vec3::unitZ(),
                     Vec3::unitX());

    expectVectorNear(Transforms::getUpAxisTransform(UpAxis::X, UpAxis::Y),
                     Vec3::unitX(),
                     Vec3::unitY());
    expectVectorNear(Transforms::getUpAxisTransform(UpAxis::X, UpAxis::Y),
                     Vec3::unitY(),
                     -Vec3::unitX());
    expectVectorNear(Transforms::getUpAxisTransform(UpAxis::X, UpAxis::Y),
                     Vec3::unitZ(),
                     Vec3::unitZ());
    expectVectorNear(Transforms::getUpAxisTransform(UpAxis::Y, UpAxis::X),
                     Vec3::unitX(),
                     -Vec3::unitY());
    expectVectorNear(Transforms::getUpAxisTransform(UpAxis::Y, UpAxis::X),
                     Vec3::unitY(),
                     Vec3::unitX());
    expectVectorNear(Transforms::getUpAxisTransform(UpAxis::Y, UpAxis::X),
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

TEST(TransformsTest, PerspectiveMatricesMatchCesiumNativeReverseZ) {
    const double horizontalFov = MathUtils::degreesToRadians(60.0);
    const double verticalFov = MathUtils::degreesToRadians(45.0);
    const double zNear = 1.0;
    const double zFar = 20000.0;

    const Mat4 byFov =
        Transforms::createPerspectiveMatrix(horizontalFov, verticalFov, zNear, zFar);

    glm::dmat4 expectedRaw(0.0);
    expectedRaw[0][0] = 1.0 / std::tan(horizontalFov * 0.5);
    expectedRaw[1][1] = -1.0 / std::tan(verticalFov * 0.5);
    expectedRaw[2][2] = zNear / (zFar - zNear);
    expectedRaw[2][3] = -1.0;
    expectedRaw[3][2] = zNear * zFar / (zFar - zNear);
    expectMatrixNear(byFov, Mat4(expectedRaw), 1e-14);

    const double right = std::tan(horizontalFov * 0.5) * zNear;
    const double top = std::tan(verticalFov * 0.5) * zNear;
    const Mat4 byFrustum = Transforms::createPerspectiveMatrix(
        -right, right, -top, top, zNear, zFar);
    expectMatrixNear(byFrustum, byFov, 1e-14);
}

TEST(TransformsTest, SkewedPerspectiveProjectionMatchesCesiumNativeMapping) {
    const double horizontalFov = MathUtils::degreesToRadians(60.0);
    const double verticalFov = MathUtils::degreesToRadians(45.0);
    const double zNear = 1.0;
    const double zFar = 20000.0;
    const double hDim = std::tan(horizontalFov * 0.5) * zNear;
    const double vDim = std::tan(verticalFov * 0.5) * zNear;

    const Mat4 symmetric = Transforms::createPerspectiveMatrix(
        -hDim,
        hDim,
        -vDim,
        vDim,
        zNear,
        zFar);
    const Mat4 skewed = Transforms::createPerspectiveMatrix(
        0.0,
        hDim,
        0.0,
        vDim,
        zNear,
        zFar);

    const glm::dvec4 point(hDim * 0.25, vDim * 0.25, -10.0, 1.0);
    glm::dvec4 symmetricProjected = symmetric.raw() * point;
    glm::dvec4 skewedProjected = skewed.raw() * point;
    symmetricProjected /= symmetricProjected.w;
    skewedProjected /= skewedProjected.w;

    EXPECT_NEAR(symmetricProjected.x,
                skewedProjected.x / 2.0 + 0.5,
                1e-14);
    EXPECT_NEAR(symmetricProjected.y,
                skewedProjected.y / 2.0 - 0.5,
                1e-14);
    EXPECT_NEAR(symmetricProjected.z, skewedProjected.z, 1e-14);
}

TEST(TransformsTest, InfinitePerspectiveMatrixMatchesCesiumNativeReverseZ) {
    const Mat4 matrix = Transforms::createPerspectiveMatrix(
        -0.5,
        0.5,
        -0.25,
        0.25,
        2.0,
        std::numeric_limits<double>::infinity());

    EXPECT_DOUBLE_EQ(4.0, matrix(0, 0));
    EXPECT_DOUBLE_EQ(-8.0, matrix(1, 1));
    EXPECT_DOUBLE_EQ(0.0, matrix(2, 2));
    EXPECT_DOUBLE_EQ(-1.0, matrix(3, 2));
    EXPECT_DOUBLE_EQ(2.0, matrix(2, 3));
}

TEST(TransformsTest, OrthographicMatrixMatchesCesiumNativeReverseZ) {
    const Mat4 finite =
        Transforms::createOrthographicMatrix(-2.0, 4.0, -3.0, 5.0, 1.0, 11.0);

    glm::dmat4 expectedRaw(1.0);
    expectedRaw[0][0] = 2.0 / 6.0;
    expectedRaw[1][1] = 2.0 / (-3.0 - 5.0);
    expectedRaw[2][2] = 1.0 / 10.0;
    expectedRaw[3][0] = -2.0 / 6.0;
    expectedRaw[3][1] = 2.0 / 8.0;
    expectedRaw[3][2] = 11.0 / 10.0;
    expectMatrixNear(finite, Mat4(expectedRaw), 1e-14);

    const Mat4 infinite = Transforms::createOrthographicMatrix(
        -2.0,
        4.0,
        -3.0,
        5.0,
        1.0,
        std::numeric_limits<double>::infinity());
    EXPECT_DOUBLE_EQ(0.0, infinite(2, 2));
    EXPECT_DOUBLE_EQ(1.0, infinite(2, 3));
}

TEST(TransformsTest, OrthographicProjectionContainsPerspectiveFrustumPoint) {
    const double horizontalFov = MathUtils::degreesToRadians(60.0);
    const double verticalFov = MathUtils::degreesToRadians(45.0);
    const double zNear = 1.0;
    const double zFar = 20000.0;
    const double hDim = std::tan(horizontalFov * 0.5) * zNear;
    const double vDim = std::tan(verticalFov * 0.5) * zNear;

    const Mat4 orthographic = Transforms::createOrthographicMatrix(
        -hDim / zNear * zFar,
        hDim / zNear * zFar,
        -vDim / zNear * zFar,
        vDim / zNear * zFar,
        zNear,
        zFar);

    const glm::dvec4 point(
        std::sin(MathUtils::degreesToRadians(20.0)) * 10000.0,
        std::sin(MathUtils::degreesToRadians(10.0)) * 10000.0,
        -10000.0,
        1.0);

    EXPECT_TRUE(pointInClipVolume(orthographic.raw() * point));
}

TEST(TransformsTest, ViewMatrixMatchesCesiumNativePoseInverse) {
    const Vec3 position(10.0, 20.0, 30.0);
    const Vec3 direction = Vec3(1.0, 2.0, -4.0).normalized();
    const Vec3 up = Vec3(0.0, 0.0, 1.0);

    const Mat4 view = Transforms::createViewMatrix(position, direction, up);

    const Vec3 forward = -direction;
    const Vec3 side = up.cross(forward).normalized();
    const Vec3 poseUp = forward.cross(side).normalized();

    EXPECT_NEAR(side.x(), view(0, 0), 1e-12);
    EXPECT_NEAR(side.y(), view(0, 1), 1e-12);
    EXPECT_NEAR(side.z(), view(0, 2), 1e-12);
    EXPECT_NEAR(poseUp.x(), view(1, 0), 1e-12);
    EXPECT_NEAR(poseUp.y(), view(1, 1), 1e-12);
    EXPECT_NEAR(poseUp.z(), view(1, 2), 1e-12);
    EXPECT_NEAR(forward.x(), view(2, 0), 1e-12);
    EXPECT_NEAR(forward.y(), view(2, 1), 1e-12);
    EXPECT_NEAR(forward.z(), view(2, 2), 1e-12);
    EXPECT_NEAR(-side.dot(position), view(0, 3), 1e-12);
    EXPECT_NEAR(-poseUp.dot(position), view(1, 3), 1e-12);
    EXPECT_NEAR(-forward.dot(position), view(2, 3), 1e-12);
}

TEST(TransformsTest, TranslationRotationScaleMatrixMatchesCesiumNative) {
    const Vec3 translation(10.0, 20.0, 30.0);
    const glm::dquat rotation =
        glm::angleAxis(MathUtils::PiOverTwo, glm::dvec3(0.0, 0.0, 1.0));
    const Vec3 scale(2.0, 3.0, 4.0);

    const Mat4 matrix =
        Transforms::createTranslationRotationScaleMatrix(
            translation,
            rotation,
            scale);

    expectVec3Near(matrix.transformVector(Vec3::unitX()),
                   Vec3(0.0, 2.0, 0.0),
                   1e-12);
    expectVec3Near(matrix.transformVector(Vec3::unitY()),
                   Vec3(-3.0, 0.0, 0.0),
                   1e-12);
    expectVec3Near(matrix.transformVector(Vec3::unitZ()),
                   Vec3(0.0, 0.0, 4.0),
                   1e-12);
    expectVec3Near(matrix.transformPoint(Vec3::zero()), translation, 1e-12);
}

TEST(TransformsTest, ComputeTranslationRotationScaleMatchesCesiumNative) {
    const Vec3 translation(-5.0, 7.0, 11.0);
    const glm::dquat rotation =
        glm::angleAxis(0.35, glm::normalize(glm::dvec3(1.0, 2.0, 3.0)));
    const Vec3 scale(-2.0, -3.0, -4.0);
    const Mat4 matrix =
        Transforms::createTranslationRotationScaleMatrix(
            translation,
            rotation,
            scale);

    Vec3 actualTranslation;
    glm::dquat actualRotation;
    Vec3 actualScale;
    Transforms::computeTranslationRotationScaleFromMatrix(
        matrix,
        &actualTranslation,
        &actualRotation,
        &actualScale);

    expectVec3Near(actualTranslation, translation, 1e-12);
    expectVec3Near(actualScale, scale, 1e-12);

    const Mat4 reconstructed =
        Transforms::createTranslationRotationScaleMatrix(
            actualTranslation,
            actualRotation,
            actualScale);
    expectMatrixNear(reconstructed, matrix, 1e-12);
}

TEST(TransformsTest, ComputeTranslationRotationScaleAllowsNullOutputs) {
    const Vec3 translation(3.0, -4.0, 5.0);
    const glm::dquat rotation =
        glm::angleAxis(0.2, glm::normalize(glm::dvec3(0.0, 1.0, 1.0)));
    const Vec3 scale(2.0, 3.0, 4.0);
    const Mat4 matrix =
        Transforms::createTranslationRotationScaleMatrix(
            translation,
            rotation,
            scale);

    Vec3 actualTranslation;
    Transforms::computeTranslationRotationScaleFromMatrix(
        matrix,
        &actualTranslation,
        nullptr,
        nullptr);
    expectVec3Near(actualTranslation, translation, 1e-12);

    glm::dquat actualRotation;
    Transforms::computeTranslationRotationScaleFromMatrix(
        matrix,
        nullptr,
        &actualRotation,
        nullptr);
    EXPECT_NEAR(std::abs(glm::dot(rotation, actualRotation)), 1.0, 1e-12);

    Vec3 actualScale;
    Transforms::computeTranslationRotationScaleFromMatrix(
        matrix,
        nullptr,
        nullptr,
        &actualScale);
    expectVec3Near(actualScale, scale, 1e-12);
}
