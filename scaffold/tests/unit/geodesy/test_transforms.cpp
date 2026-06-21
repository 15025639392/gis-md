#include <gtest/gtest.h>

#include "earth_engine/core/geodesy/Transforms.h"
#include "earth_engine/core/geodesy/Cartographic.h"
#include "earth_engine/core/geodesy/Ellipsoid.h"
#include "earth_engine/core/math/MathUtils.h"
#include "earth_engine/core/math/Mat4.h"
#include "earth_engine/core/math/Vec3.h"

#include <algorithm>
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

Vec3 columnVector(const Mat4& matrix, int col) {
    return Vec3(matrix(0, col), matrix(1, col), matrix(2, col));
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

template <typename Callback>
void forEachCesiumFrustumSample(double horizontalFov,
                                double verticalFov,
                                double zNear,
                                double zFar,
                                Callback callback) {
    for (int i = 0; i < 11; ++i) {
        double horizontalAngle =
            -horizontalFov * 0.5 + i * horizontalFov / 10.0;
        horizontalAngle = std::clamp(horizontalAngle,
                                     -horizontalFov + 0.1,
                                     horizontalFov - 0.1);
        const double sinHorizontal = std::sin(horizontalAngle);
        for (int j = 0; j < 10; ++j) {
            double verticalAngle =
                -verticalFov * 0.5 + j * verticalFov / 10.0;
            verticalAngle = std::clamp(verticalAngle,
                                       -verticalFov + 0.1,
                                       verticalFov - 0.1);
            const double sinVertical = std::sin(verticalAngle);
            for (int k = 0; k < 10; ++k) {
                double depth = zNear + k * (zFar - zNear) / 10.0;
                depth = std::clamp(depth, zNear + 0.1, zFar - 0.1);
                callback(glm::dvec4(sinHorizontal * depth,
                                    sinVertical * depth,
                                    -depth,
                                    1.0));
            }
        }
    }
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

TEST(TransformsTest, EastNorthUpToFixedFrameMatchesCesiumNativeSpecialCases) {
    // Ported from cesium-native CesiumGeospatial/src/GlobeTransforms.cpp:
    // the ellipsoid center and both poles have explicit local-frame branches.
    const Mat4 zeroFrame = Transforms::eastNorthUpToFixedFrame(Vec3::zero());
    expectVec3Near(columnVector(zeroFrame, 0), Vec3(0.0, 1.0, 0.0), 1e-12);
    expectVec3Near(columnVector(zeroFrame, 1), Vec3(-1.0, 0.0, 0.0), 1e-12);
    expectVec3Near(columnVector(zeroFrame, 2), Vec3(0.0, 0.0, 1.0), 1e-12);
    expectVec3Near(columnVector(zeroFrame, 3), Vec3::zero(), 1e-12);

    const double polarRadius = Ellipsoid::WGS84().semiMinorAxis();
    const Mat4 northPoleFrame =
        Transforms::eastNorthUpToFixedFrame(Vec3(0.0, 0.0, polarRadius));
    expectVec3Near(columnVector(northPoleFrame, 0), Vec3(0.0, 1.0, 0.0), 1e-12);
    expectVec3Near(columnVector(northPoleFrame, 1), Vec3(-1.0, 0.0, 0.0), 1e-12);
    expectVec3Near(columnVector(northPoleFrame, 2), Vec3(0.0, 0.0, 1.0), 1e-12);
    expectVec3Near(columnVector(northPoleFrame, 3),
                   Vec3(0.0, 0.0, polarRadius),
                   1e-12);

    const Mat4 southPoleFrame =
        Transforms::eastNorthUpToFixedFrame(Vec3(0.0, 0.0, -polarRadius));
    expectVec3Near(columnVector(southPoleFrame, 0), Vec3(0.0, 1.0, 0.0), 1e-12);
    expectVec3Near(columnVector(southPoleFrame, 1), Vec3(1.0, 0.0, 0.0), 1e-12);
    expectVec3Near(columnVector(southPoleFrame, 2), Vec3(0.0, 0.0, -1.0), 1e-12);
    expectVec3Near(columnVector(southPoleFrame, 3),
                   Vec3(0.0, 0.0, -polarRadius),
                   1e-12);
}

TEST(TransformsTest, EastNorthUpToFixedFrameMatchesCesiumNativeGeneralCase) {
    // Cesium derives east from the ECEF xy-plane, up from the ellipsoid
    // geodetic normal, and north as cross(up, east).
    const Vec3 origin = Ellipsoid::WGS84().cartographicToCartesian(
        Cartographic::fromDegrees(116.397, 39.908, 50.0));

    const Mat4 frame = Transforms::eastNorthUpToFixedFrame(origin);
    const Vec3 east = Vec3(-origin.y(), origin.x(), 0.0).normalized();
    const Vec3 up = Ellipsoid::WGS84().geodeticSurfaceNormal(origin);
    const Vec3 north = up.cross(east);

    expectVec3Near(columnVector(frame, 0), east, 1e-12);
    expectVec3Near(columnVector(frame, 1), north, 1e-12);
    expectVec3Near(columnVector(frame, 2), up, 1e-12);
    expectVec3Near(columnVector(frame, 3), origin, 1e-12);
}

TEST(TransformsTest, EcefEnuRoundtripUsesEastNorthUpFrame) {
    const Cartographic origin = Cartographic::fromDegrees(116.397, 39.908, 50.0);
    const Vec3 localPoint(12.5, -30.0, 4.0);

    const Mat4 enuToEcef = Transforms::enuToEcef(origin);
    const Mat4 ecefToEnu = Transforms::ecefToEnu(origin);

    expectVec3Near(ecefToEnu * (enuToEcef * localPoint), localPoint, 1e-6);
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

TEST(TransformsTest, PerspectiveProjectionContainsFrustumSamples) {
    // Ported from cesium-native CesiumGeometry/test/TestTransforms.cpp:
    // points inside the camera frustum must map into Vulkan-style reverse-Z
    // clip coordinates: -w..w in x/y and 0..w in z.
    const double horizontalFov = MathUtils::degreesToRadians(60.0);
    const double verticalFov = MathUtils::degreesToRadians(45.0);
    const double zNear = 1.0;
    const double zFar = 20000.0;
    const Mat4 projection =
        Transforms::createPerspectiveMatrix(horizontalFov,
                                            verticalFov,
                                            zNear,
                                            zFar);

    forEachCesiumFrustumSample(
        horizontalFov,
        verticalFov,
        zNear,
        zFar,
        [&projection](const glm::dvec4& sample) {
            EXPECT_TRUE(pointInClipVolume(projection.raw() * sample));
        });
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

    forEachCesiumFrustumSample(
        horizontalFov,
        verticalFov,
        zNear,
        zFar,
        [&symmetric, &skewed](const glm::dvec4& sample) {
            glm::dvec4 skewedProjected = skewed.raw() * sample;
            if (!pointInClipVolume(skewedProjected)) {
                return;
            }

            glm::dvec4 symmetricProjected = symmetric.raw() * sample;
            skewedProjected /= skewedProjected.w;
            symmetricProjected /= symmetricProjected.w;

            EXPECT_NEAR(symmetricProjected.x,
                        skewedProjected.x / 2.0 + 0.5,
                        1e-14);
            EXPECT_NEAR(symmetricProjected.y,
                        skewedProjected.y / 2.0 - 0.5,
                        1e-14);
            EXPECT_NEAR(symmetricProjected.z, skewedProjected.z, 1e-14);
        });
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

TEST(TransformsTest, InfinitePerspectiveFovMatrixMatchesCesiumNativeReverseZ) {
    // Source-derived from cesium-native Transforms::createPerspectiveMatrix:
    // the fov overload has the same explicit infinite-far branch as the
    // frustum overload.
    const double horizontalFov = MathUtils::degreesToRadians(60.0);
    const double verticalFov = MathUtils::degreesToRadians(45.0);
    const Mat4 matrix = Transforms::createPerspectiveMatrix(
        horizontalFov,
        verticalFov,
        2.0,
        std::numeric_limits<double>::infinity());

    EXPECT_DOUBLE_EQ(1.0 / std::tan(horizontalFov * 0.5), matrix(0, 0));
    EXPECT_DOUBLE_EQ(-1.0 / std::tan(verticalFov * 0.5), matrix(1, 1));
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

    forEachCesiumFrustumSample(
        horizontalFov,
        verticalFov,
        zNear,
        zFar,
        [&orthographic](const glm::dvec4& sample) {
            EXPECT_TRUE(pointInClipVolume(orthographic.raw() * sample));
        });
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
