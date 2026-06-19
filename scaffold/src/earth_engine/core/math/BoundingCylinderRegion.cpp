#include "BoundingCylinderRegion.h"

#include "AxisAlignedBox.h"
#include "MathUtils.h"
#include "../geodesy/Transforms.h"

#include <algorithm>
#include <cmath>
#include <glm/common.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/trigonometric.hpp>

namespace earth_engine {

namespace {

OrientedBoundingBox computeBoxFromCylinderRegion(
    const Vec3& translation,
    const glm::dquat& rotation,
    double height,
    const glm::dvec2& radialBounds,
    const glm::dvec2& angularBounds) {
    const double innerRadius = radialBounds.x;
    const double outerRadius = radialBounds.y;
    const double angleMin = angularBounds.x;
    const double angleMax = angularBounds.y;
    const bool angleReversed = angleMax < angleMin;

    const bool angleCrossesNegativePiOverTwo =
        angleReversed ? angleMin <= -MathUtils::PiOverTwo ||
                            angleMax >= -MathUtils::PiOverTwo
                      : angleMin <= -MathUtils::PiOverTwo &&
                            -MathUtils::PiOverTwo <= angleMax;
    const bool angleCrossesPiOverTwo =
        angleReversed ? angleMin <= MathUtils::PiOverTwo ||
                            angleMax >= MathUtils::PiOverTwo
                      : angleMin <= MathUtils::PiOverTwo &&
                            MathUtils::PiOverTwo <= angleMax;

    const double yMin = angleCrossesNegativePiOverTwo
        ? -1.0
        : std::min(std::sin(angleMin), std::sin(angleMax));
    const double yMax = angleCrossesPiOverTwo
        ? 1.0
        : std::max(std::sin(angleMin), std::sin(angleMax));

    const int angleMaxSign = static_cast<int>(MathUtils::sign(angleMax));
    const int angleMinSign = static_cast<int>(MathUtils::sign(angleMin));
    bool angleCrossesZero = angleReversed ? angleMinSign == angleMaxSign
                                          : angleMinSign != angleMaxSign;
    angleCrossesZero = angleCrossesZero ||
                       angleMinSign == 0 ||
                       angleMaxSign == 0;

    const double xMin = angleReversed
        ? -1.0
        : std::min(std::cos(angleMin), std::cos(angleMax));
    const double xMax = angleCrossesZero
        ? 1.0
        : std::max(std::cos(angleMin), std::cos(angleMax));

    const double zScale = height * 0.5;
    const AxisAlignedBox aabb = AxisAlignedBox::fromPositions({
        Vec3(innerRadius * xMin, innerRadius * yMin, -zScale),
        Vec3(innerRadius * xMax, innerRadius * yMax, zScale),
        Vec3(outerRadius * xMin, outerRadius * yMin, -zScale),
        Vec3(outerRadius * xMax, outerRadius * yMax, zScale)});

    const OrientedBoundingBox obb = OrientedBoundingBox::fromAxisAligned(aabb);
    const Mat4 transform = Transforms::createTranslationRotationScaleMatrix(
        translation,
        rotation,
        Vec3(1.0, 1.0, 1.0));
    return obb.transform(transform);
}

} // namespace

BoundingCylinderRegion::BoundingCylinderRegion(
    const Vec3& translation,
    const glm::dquat& rotation,
    double height,
    const glm::dvec2& radialBounds,
    const glm::dvec2& angularBounds)
    : translation_(translation),
      rotation_(rotation),
      height_(height),
      radialBounds_(radialBounds),
      angularBounds_(angularBounds),
      box_(computeBoxFromCylinderRegion(
          translation,
          rotation,
          height,
          radialBounds,
          angularBounds)) {}

BoundingCylinderRegion BoundingCylinderRegion::transform(
    const glm::dmat4& transformation) const noexcept {
    const Mat4 originalTransform = Transforms::createTranslationRotationScaleMatrix(
        translation_,
        rotation_,
        Vec3(1.0, 1.0, 1.0));
    const Mat4 transform(Mat4(transformation) * originalTransform);

    Vec3 translation;
    glm::dquat rotation(1.0, 0.0, 0.0, 0.0);
    Vec3 scale(1.0, 1.0, 1.0);
    Transforms::computeTranslationRotationScaleFromMatrix(
        transform,
        &translation,
        &rotation,
        &scale);

    const double radiusScale = std::max(scale.x(), scale.y());
    return BoundingCylinderRegion(
        translation,
        rotation,
        height_ * scale.z(),
        radialBounds_ * radiusScale,
        angularBounds_);
}

} // namespace earth_engine
