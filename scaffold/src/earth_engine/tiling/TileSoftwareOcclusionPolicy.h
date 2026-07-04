#pragma once

#include "TileOcclusionState.h"

#include "../core/math/Vec3.h"

namespace earth_engine {

struct TilesetTile;

class TileSoftwareOcclusionPolicy {
public:
    /// 相机侧派生量（测地坐标 + 椭球缩放坐标）。相机在一帧内不变，而
    /// cartesianToCartographic 是迭代法测地转换——按帧构造一次复用，
    /// 避免每瓦片重复转换（性能审计 P2-11）。
    struct CameraContext {
        Vec3 cameraPosition = Vec3::zero();
        double cameraLongitude = 0.0;
        double cameraLatitude = 0.0;
        double cameraHeight = 0.0;
        Vec3 cameraScaled = Vec3::zero();
        double vhMagnitudeSquared = 0.0;

        static CameraContext fromCameraPosition(const Vec3& cameraPosition);
    };

    static TileOcclusionState check(const TilesetTile& tile,
                                    const CameraContext& camera);

    /// 便捷重载：内部现场构造 CameraContext（单次调用/测试用；逐瓦片
    /// 热路径应复用外部构造的 CameraContext）。
    static TileOcclusionState check(const TilesetTile& tile,
                                    const Vec3& cameraPosition);
};

} // namespace earth_engine
