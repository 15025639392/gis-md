#pragma once

#include "MathUtils.h"
#include "OrientedBoundingBox.h"
#include "Plane.h"
#include "Vec3.h"

#include <glm/gtc/quaternion.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>

namespace earth_engine {

/// Bounding volume for 3DTILES_bounding_volume_cylinder regions.
/// The cylinder is approximated by a tight OBB, matching cesium-native.
class BoundingCylinderRegion {
public:
    BoundingCylinderRegion(
        const Vec3& translation,
        const glm::dquat& rotation,
        double height,
        const glm::dvec2& radialBounds,
        const glm::dvec2& angularBounds =
            glm::dvec2(-MathUtils::OnePi, MathUtils::OnePi));

    const Vec3& getCenter() const noexcept { return box_.getCenter(); }
    const Vec3& getTranslation() const noexcept { return translation_; }
    const glm::dquat& getRotation() const noexcept { return rotation_; }
    double getHeight() const noexcept { return height_; }
    const glm::dvec2& getRadialBounds() const noexcept { return radialBounds_; }
    const glm::dvec2& getAngularBounds() const noexcept { return angularBounds_; }

    int intersectPlane(const Plane& plane) const noexcept {
        return box_.intersectPlane(plane);
    }
    double computeDistanceSquaredToPosition(const Vec3& position) const noexcept {
        return box_.computeDistanceSquaredToPosition(position);
    }
    bool contains(const Vec3& position) const noexcept {
        return box_.contains(position);
    }

    BoundingCylinderRegion transform(const glm::dmat4& transformation) const noexcept;
    OrientedBoundingBox toOrientedBoundingBox() const noexcept { return box_; }

private:
    Vec3 translation_;
    glm::dquat rotation_;
    double height_ = 0.0;
    glm::dvec2 radialBounds_;
    glm::dvec2 angularBounds_;
    OrientedBoundingBox box_;
};

} // namespace earth_engine
