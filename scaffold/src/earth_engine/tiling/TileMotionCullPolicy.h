#pragma once

#include "TileBoundingVolume.h"
#include "TilesetTile.h"
#include "../core/math/OrientedBoundingBox.h"
#include "../core/math/Vec3.h"

#include <algorithm>
#include <optional>

namespace earth_engine {

// cesium-js cullRequestsWhileMoving / isOnScreenLongEnough 的忠实移植。
//
// 语义:相机运动期间,若某瓦片本帧在屏幕上划过的距离超过它自身尺寸,则
// "回来时多半已经不在视野里",本帧不发它的网络请求(下一帧重新评估)。这是
// 对 replace-refine 地形 globe 真正有效的运动降载杠杆(foveated 对地形是
// no-op,cesium 故意用 `replace && !skipLOD` guard 关掉了它)。
//
//   diameter      = max(2 × boundingSphereRadius, 1)
//   movementRatio = multiplier × cameraPositionDeltaMagnitude / diameter
//   onScreenLongEnough = movementRatio < 1.0    // cesium 原式
//   deferForMotion     = !onScreenLongEnough
//
// multiplier(cesium 默认 60)越大剔除越激进。cameraPositionDeltaMagnitude 与
// boundingSphereRadius 必须同处 ECEF 世界空间(米)。
class TileMotionCullPolicy {
public:
    struct Input {
        bool cullRequestsWhileMoving = false;
        double cullRequestsWhileMovingMultiplier = 60.0;
        double cameraPositionDeltaMagnitude = 0.0;
        double boundingSphereRadius = 0.0;
    };

    static bool shouldDeferForMotion(const Input& in) {
        if (!in.cullRequestsWhileMoving) {
            return false;
        }
        // 无有效包围体半径时不剔除(否则 diameter 退化为 1 会误判恒剔除)。
        if (!(in.boundingSphereRadius > 0.0)) {
            return false;
        }
        const double diameter =
            std::max(in.boundingSphereRadius * 2.0, 1.0);
        const double movementRatio =
            (in.cullRequestsWhileMovingMultiplier *
             in.cameraPositionDeltaMagnitude) /
            diameter;
        return movementRatio >= 1.0;
    }

    // cesium BoundingSphere.fromOrientedBoundingBox:半径 = |a0 + a1 + a2|
    // (三个半轴向量之和的模)。Sphere 直接取半径;Region/其它 kind 先转 OBB。
    static double boundingSphereRadius(const TilesetTile& tile) {
        if (!tile.boundingVolume) {
            return 0.0;
        }
        const TileBoundingVolume& volume = *tile.boundingVolume;
        if (volume.kind == TileBoundingVolumeKind::Sphere) {
            return volume.sphere.getRadius();
        }
        const std::optional<OrientedBoundingBox> obb =
            volume.toOrientedBoundingBox();
        if (!obb) {
            return 0.0;
        }
        const Vec3 sum = obb->getHalfAxis(0) + obb->getHalfAxis(1) +
                         obb->getHalfAxis(2);
        return sum.length();
    }
};

} // namespace earth_engine
