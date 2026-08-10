#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace earth_engine {

class Camera;

/// 相机位姿的纯几何操作。无状态、不认识地球/地形/手势——只是把一个旋转
/// 施加到 (eye, direction, up) 上。
///
/// 之所以是自由函数而不是某个类的成员：手势侧（GlobeGestureManipulator）与
/// 编排侧（resetNorthUp / scriptedPan）都要用同一份数学，放进任一方都会让
/// 另一方产生一条本不该有的依赖。
namespace camera_ops {

/// 绕地心旋转整个相机（eye 一起转 ⇒ 相对地球的位姿变，视线方向随之改变）。
void rotateAboutOrigin(Camera& camera, const glm::dquat& delta);

/// 绕任意世界点 center 旋转相机。center 在旋转轴上 ⇒ 该点的屏幕位置不变，
/// 这正是双指 twist / pitch 的保锚基础。轴长或角度退化时不做任何事。
void rotateAboutPoint(Camera& camera,
                      const glm::dvec3& center,
                      const glm::dvec3& axis,
                      double angle);

}  // namespace camera_ops
}  // namespace earth_engine
