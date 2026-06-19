#pragma once

#include "Cartographic.h"
#include "../math/Vec3.h"
#include "../math/Mat4.h"

#include <glm/fwd.hpp>

namespace earth_engine {

class Ellipsoid;

enum class UpAxis {
    X,
    Y,
    Z
};

/// 坐标变换工具集。
/// 提供 ECEF ↔ ENU、degree ↔ radian 等转换。
class Transforms {
public:
    static const Mat4& X_UP_TO_Y_UP();
    static const Mat4& X_UP_TO_Z_UP();
    static const Mat4& Y_UP_TO_X_UP();
    static const Mat4& Y_UP_TO_Z_UP();
    static const Mat4& Z_UP_TO_X_UP();
    static const Mat4& Z_UP_TO_Y_UP();
    static const Mat4& getUpAxisTransform(UpAxis from, UpAxis to);

    /// Creates translation * rotation * scale, matching
    /// cesium-native CesiumGeometry::Transforms.
    static Mat4 createTranslationRotationScaleMatrix(
        const Vec3& translation,
        const glm::dquat& rotation,
        const Vec3& scale);

    /// Decomposes a translation-rotation-scale matrix. Scale may be negative.
    static void computeTranslationRotationScaleFromMatrix(
        const Mat4& matrix,
        Vec3* translation,
        glm::dquat* rotation,
        Vec3* scale);

    /// Vulkan-style reverse-Z perspective projection, matching
    /// cesium-native CesiumGeometry::Transforms::createPerspectiveMatrix.
    static Mat4 createPerspectiveMatrix(double horizontalFovRadians,
                                        double verticalFovRadians,
                                        double zNear,
                                        double zFar);
    static Mat4 createPerspectiveMatrix(double left,
                                        double right,
                                        double bottom,
                                        double top,
                                        double zNear,
                                        double zFar);

    /// Vulkan-style reverse-Z orthographic projection, matching
    /// cesium-native CesiumGeometry::Transforms::createOrthographicMatrix.
    static Mat4 createOrthographicMatrix(double left,
                                         double right,
                                         double bottom,
                                         double top,
                                         double zNear,
                                         double zFar);

    /// View matrix from viewer pose, matching
    /// cesium-native CesiumGeometry::Transforms::createViewMatrix.
    static Mat4 createViewMatrix(const Vec3& position,
                                 const Vec3& direction,
                                 const Vec3& up);

    /// 计算从局部 East-North-Up 到 ECEF fixed frame 的矩阵。
    /// 语义对齐 cesium-native GlobeTransforms::eastNorthUpToFixedFrame。
    static Mat4 eastNorthUpToFixedFrame(const Vec3& originEcef);
    static Mat4 eastNorthUpToFixedFrame(const Vec3& originEcef,
                                        const Ellipsoid& ellipsoid);

    /// 计算从 ECEF 到 ENU（East-North-Up）的旋转矩阵。
    /// @param origin 局部坐标系原点（大地坐标）
    /// @return 4×4 矩阵，将 ECEF 点变换到 ENU 空间
    static Mat4 ecefToEnu(const Cartographic& origin);

    /// ENU 到 ECEF（逆变换）
    static Mat4 enuToEcef(const Cartographic& origin);

    /// degree → radian
    static double toRadians(double deg);

    /// radian → degree
    static double toDegrees(double rad);
};

} // namespace earth_engine
