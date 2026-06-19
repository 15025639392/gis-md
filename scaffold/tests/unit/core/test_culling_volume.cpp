#include <gtest/gtest.h>

#include "earth_engine/core/math/CullingVolume.h"

#include <glm/gtc/constants.hpp>

#include <cmath>

using namespace earth_engine;

namespace {

bool planeNear(const Plane& lhs, const Plane& rhs, double epsilon) {
    return lhs.getNormal().distanceTo(rhs.getNormal()) <= epsilon &&
           std::abs(lhs.getDistance() - rhs.getDistance()) <= epsilon;
}

void expectCullingVolumeNear(const CullingVolume& lhs,
                             const CullingVolume& rhs,
                             double epsilon) {
    EXPECT_TRUE(planeNear(lhs.leftPlane, rhs.leftPlane, epsilon));
    EXPECT_TRUE(planeNear(lhs.rightPlane, rhs.rightPlane, epsilon));
    EXPECT_TRUE(planeNear(lhs.topPlane, rhs.topPlane, epsilon));
    EXPECT_TRUE(planeNear(lhs.bottomPlane, rhs.bottomPlane, epsilon));
}

Mat4 cesiumPerspectiveMatrix(double fovX,
                             double fovY,
                             double nearPlane,
                             double farPlane) {
    glm::dmat4 projection(0.0);
    projection[0][0] = 1.0 / std::tan(fovX * 0.5);
    projection[1][1] = -1.0 / std::tan(fovY * 0.5);
    projection[2][2] = nearPlane / (farPlane - nearPlane);
    projection[2][3] = -1.0;
    projection[3][2] = nearPlane * farPlane / (farPlane - nearPlane);
    return Mat4(projection);
}

Mat4 cesiumPerspectiveOffCenterMatrix(double left,
                                      double right,
                                      double bottom,
                                      double top,
                                      double nearPlane) {
    glm::dmat4 projection(0.0);
    projection[0][0] = 2.0 * nearPlane / (right - left);
    projection[1][1] = 2.0 * nearPlane / (bottom - top);
    projection[2][0] = (right + left) / (right - left);
    projection[2][1] = (bottom + top) / (bottom - top);
    projection[2][3] = -1.0;
    projection[3][2] = nearPlane;
    return Mat4(projection);
}

Mat4 cesiumOrthographicMatrix(double left,
                              double right,
                              double bottom,
                              double top) {
    glm::dmat4 projection(1.0);
    projection[0][0] = 2.0 / (right - left);
    projection[1][1] = 2.0 / (bottom - top);
    projection[2][2] = 0.0;
    projection[3][0] = -(right + left) / (right - left);
    projection[3][1] = -(bottom + top) / (bottom - top);
    projection[3][2] = 1.0;
    return Mat4(projection);
}

Mat4 cesiumViewMatrix(const Vec3& position, const Vec3& direction, const Vec3& up) {
    const Vec3 forward = direction * -1.0;
    const Vec3 side = up.cross(forward).normalized();
    const Vec3 poseUp = forward.cross(side).normalized();

    glm::dmat4 view(1.0);
    view[0][0] = side.x();
    view[1][0] = side.y();
    view[2][0] = side.z();
    view[0][1] = poseUp.x();
    view[1][1] = poseUp.y();
    view[2][1] = poseUp.z();
    view[0][2] = forward.x();
    view[1][2] = forward.y();
    view[2][2] = forward.z();
    view[3][0] = -side.dot(position);
    view[3][1] = -poseUp.dot(position);
    view[3][2] = -forward.dot(position);
    return Mat4(view);
}

} // namespace

TEST(CullingVolumeTest, PerspectiveFieldOfViewDoesNotCrashFarFromOrigin) {
    EXPECT_NO_THROW(CullingVolume::fromPerspectiveFieldOfView(
        Vec3(1e20, 1e20, 1e20),
        Vec3(0.0, 0.0, 1.0),
        Vec3(0.0, 1.0, 0.0),
        glm::half_pi<double>(),
        glm::half_pi<double>()));
}

TEST(CullingVolumeTest, PerspectiveFieldOfViewDoesNotCrashAtOrigin) {
    EXPECT_NO_THROW(CullingVolume::fromPerspectiveFieldOfView(
        Vec3::zero(),
        Vec3(0.0, 0.0, 1.0),
        Vec3(0.0, 1.0, 0.0),
        glm::half_pi<double>(),
        glm::half_pi<double>()));
}

TEST(CullingVolumeTest, FieldOfViewAndClipMatrixConstructorsMatchCesiumNative) {
    const Vec3 position(1e5, 1e5, 1e5);
    const Vec3 direction(0.0, 0.0, 1.0);
    const Vec3 up(0.0, 1.0, 0.0);

    const CullingVolume fromFov =
        CullingVolume::fromPerspectiveFieldOfView(
            position,
            direction,
            up,
            glm::half_pi<double>(),
            glm::half_pi<double>());

    const Mat4 clipMatrix =
        cesiumPerspectiveMatrix(glm::half_pi<double>(),
                                glm::half_pi<double>(),
                                10.0,
                                200000.0) *
        cesiumViewMatrix(position, direction, up);
    const CullingVolume fromClip = CullingVolume::fromClipMatrix(clipMatrix);

    expectCullingVolumeNear(fromFov, fromClip, 1e-10);
}

TEST(CullingVolumeTest, IdentityClipMatrixExtractsCesiumNativePlaneSigns) {
    const CullingVolume volume = CullingVolume::fromClipMatrix(Mat4::identity());

    EXPECT_TRUE(planeNear(volume.leftPlane,
                          Plane(Vec3(1.0, 0.0, 0.0), 1.0),
                          0.0));
    EXPECT_TRUE(planeNear(volume.rightPlane,
                          Plane(Vec3(-1.0, 0.0, 0.0), 1.0),
                          0.0));
    EXPECT_TRUE(planeNear(volume.topPlane,
                          Plane(Vec3(0.0, 1.0, 0.0), 1.0),
                          0.0));
    EXPECT_TRUE(planeNear(volume.bottomPlane,
                          Plane(Vec3(0.0, -1.0, 0.0), 1.0),
                          0.0));
}

TEST(CullingVolumeTest, PerspectiveOffCenterMatchesCesiumNativeClipMatrix) {
    const Vec3 position(1234.0, -5678.0, 91011.0);
    const Vec3 direction(0.25, -0.4, 1.0);
    const Vec3 up(0.0, 1.0, 0.0);
    const double left = -1.5;
    const double right = 2.0;
    const double bottom = -0.75;
    const double top = 1.25;
    const double nearPlane = 3.0;

    const CullingVolume fromOffCenter =
        CullingVolume::fromPerspectiveOffCenter(
            position,
            direction.normalized(),
            up,
            left,
            right,
            bottom,
            top,
            nearPlane);

    const Mat4 clipMatrix =
        cesiumPerspectiveOffCenterMatrix(
            left,
            right,
            bottom,
            top,
            nearPlane) *
        cesiumViewMatrix(position, direction.normalized(), up);
    const CullingVolume fromClip = CullingVolume::fromClipMatrix(clipMatrix);

    expectCullingVolumeNear(fromOffCenter, fromClip, 1e-10);
}

TEST(CullingVolumeTest, OrthographicMatchesCesiumNativeClipMatrix) {
    const Vec3 position(-500.0, 250.0, 1000.0);
    const Vec3 direction(0.0, 0.0, -1.0);
    const Vec3 up(0.0, 1.0, 0.0);
    const double left = -10.0;
    const double right = 20.0;
    const double bottom = -5.0;
    const double top = 15.0;
    const double nearPlane = 2.0;

    const CullingVolume fromOrthographic =
        CullingVolume::fromOrthographicOffCenter(
            position,
            direction,
            up,
            left,
            right,
            bottom,
            top,
            nearPlane);

    const Mat4 clipMatrix =
        cesiumOrthographicMatrix(left, right, bottom, top) *
        cesiumViewMatrix(position, direction, up);
    const CullingVolume fromClip = CullingVolume::fromClipMatrix(clipMatrix);

    expectCullingVolumeNear(fromOrthographic, fromClip, 1e-10);
}
