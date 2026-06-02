#pragma once

#include "TileKey.h"
#include <vector>
#include <memory>

namespace earth_engine {

class TileScheme;
class Camera;

/// 一帧的可见瓦片计算结果。
struct TilePlan {
    /// 当前目标 zoom 层级
    int zoom = 0;

    /// 当前帧需要的瓦片键列表
    std::vector<TileKey> visibleTiles;

    /// 上一 zoom 层级（parent fallback 用）
    std::vector<TileKey> parentTiles;
};

/// 瓦片计划计算器。
/// 根据相机状态和瓦片体系，计算当前帧应该显示哪些瓦片。
class TilePlanBuilder {
public:
    /// 计算可见瓦片集合。
    /// @param camera 当前相机
    /// @param scheme 瓦片体系
    /// @param viewportWidthPixels 视口宽度
    /// @param viewportHeightPixels 视口高度
    /// @return 瓦片计划（包含目标 zoom 和 visibleTiles）
    static TilePlan compute(const Camera& camera,
                            const TileScheme& scheme,
                            double viewportWidthPixels,
                            double viewportHeightPixels,
                            int previousZoom = -1);

    /// 根据相机距地表高度估算合适的 zoom 层级。
    /// @param cameraHeightMeters 相机距椭球面的高度（米）
    /// @param viewportHeightPixels 视口高度（像素）
    /// @param verticalFovRadians 垂直视场角
    /// @param tileSize tile 的像素尺寸
    /// @return 建议的 zoom 层级（clamped 到 [minZoom, maxZoom]）
    static int zoomLevelFromHeight(double cameraHeightMeters,
                                   double viewportHeightPixels,
                                   double verticalFovRadians,
                                   int tileSize,
                                   int minZoom,
                                   int maxZoom,
                                   int previousZoom = -1);
};

} // namespace earth_engine
