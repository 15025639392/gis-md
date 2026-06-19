#include "CullingVolume.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace earth_engine {

namespace {

Plane normalizedPlane(double a, double b, double c, double d) {
    const double length = std::sqrt(a * a + b * b + c * c);
    return Plane(Vec3(a / length, b / length, c / length), d / length);
}

} // namespace

CullingVolume CullingVolume::fromPerspectiveFieldOfView(
    const Vec3& position,
    const Vec3& direction,
    const Vec3& up,
    double horizontalFovRadians,
    double verticalFovRadians) {
    const double top = std::tan(0.5 * verticalFovRadians);
    const double bottom = -top;
    const double rightEdge = std::tan(0.5 * horizontalFovRadians);
    const double leftEdge = -rightEdge;

    const double positionLength = position.length();
    const double nearPlane = std::max(
        1.0,
        std::nextafter(positionLength, std::numeric_limits<double>::max()) -
            positionLength);
    const Vec3 right = direction.cross(up);
    const Vec3 nearCenter = position + direction * nearPlane;

    Vec3 normal = (nearCenter + right * leftEdge - position).normalized();
    normal = normal.cross(up).normalized();
    const Plane leftPlane(normal, -normal.dot(position));

    normal = nearCenter + right * rightEdge - position;
    normal = up.cross(normal).normalized();
    const Plane rightPlane(normal, -normal.dot(position));

    normal = nearCenter + up * bottom - position;
    normal = right.cross(normal).normalized();
    const Plane bottomPlane(normal, -normal.dot(position));

    normal = nearCenter + up * top - position;
    normal = normal.cross(right).normalized();
    const Plane topPlane(normal, -normal.dot(position));

    return CullingVolume{leftPlane, rightPlane, topPlane, bottomPlane};
}

CullingVolume CullingVolume::fromPerspectiveOffCenter(
    const Vec3& position,
    const Vec3& direction,
    const Vec3& up,
    double left,
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

    return fromClipMatrix(Mat4(projection * view));
}

CullingVolume CullingVolume::fromClipMatrix(const Mat4& clipMatrix) {
    const Plane leftPlane = normalizedPlane(
        clipMatrix(3, 0) + clipMatrix(0, 0),
        clipMatrix(3, 1) + clipMatrix(0, 1),
        clipMatrix(3, 2) + clipMatrix(0, 2),
        clipMatrix(3, 3) + clipMatrix(0, 3));
    const Plane rightPlane = normalizedPlane(
        clipMatrix(3, 0) - clipMatrix(0, 0),
        clipMatrix(3, 1) - clipMatrix(0, 1),
        clipMatrix(3, 2) - clipMatrix(0, 2),
        clipMatrix(3, 3) - clipMatrix(0, 3));
    const Plane bottomPlane = normalizedPlane(
        clipMatrix(3, 0) - clipMatrix(1, 0),
        clipMatrix(3, 1) - clipMatrix(1, 1),
        clipMatrix(3, 2) - clipMatrix(1, 2),
        clipMatrix(3, 3) - clipMatrix(1, 3));
    const Plane topPlane = normalizedPlane(
        clipMatrix(3, 0) + clipMatrix(1, 0),
        clipMatrix(3, 1) + clipMatrix(1, 1),
        clipMatrix(3, 2) + clipMatrix(1, 2),
        clipMatrix(3, 3) + clipMatrix(1, 3));

    return CullingVolume{leftPlane, rightPlane, topPlane, bottomPlane};
}

} // namespace earth_engine
