#include "CameraPoseOps.h"

#include "../core/math/Vec3.h"
#include "../scene/Camera.h"

#include <cmath>

namespace earth_engine {
namespace camera_ops {

void rotateAboutOrigin(Camera& camera, const glm::dquat& delta) {
    const glm::dvec3 eye = delta * camera.position().raw();
    const glm::dvec3 direction = delta * camera.direction().raw();
    const glm::dvec3 up = delta * camera.up().raw();
    camera.setView(Vec3(eye), Vec3(direction), Vec3(up));
}

void rotateAboutPoint(Camera& camera,
                      const glm::dvec3& center,
                      const glm::dvec3& axis,
                      double angle) {
    if (glm::length(axis) < 1e-10 || std::abs(angle) < 1e-12) {
        return;
    }
    const glm::dquat delta = glm::angleAxis(angle, glm::normalize(axis));
    const glm::dvec3 eye = center + delta * (camera.position().raw() - center);
    const glm::dvec3 direction = delta * camera.direction().raw();
    const glm::dvec3 up = delta * camera.up().raw();
    camera.setView(Vec3(eye), Vec3(direction), Vec3(up));
}

}  // namespace camera_ops
}  // namespace earth_engine
