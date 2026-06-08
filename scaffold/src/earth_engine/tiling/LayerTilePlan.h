#pragma once

#include "TilePlan.h"
#include "TileGroupKey.h"
#include <vector>
#include <unordered_map>

namespace earth_engine {

class TileScheme;
class Camera;

/// 按 TileGroupKey 分组，为每组计算一次 TilePlan。
/// 用于 BasemapLayerStack::update() 中。
class TilePlanGroupBuilder {
public:
    /// 为一批图层计算分组 TilePlan。
    /// @param schemes 每个图层的 scheme（按图层索引）
    /// @param camera 当前相机
    /// @param viewportWidthPixels 视口宽度
    /// @param viewportHeightPixels 视口高度
    /// @return groupKey → TilePlan 的映射（每组计算一次）
    static std::unordered_map<TileGroupKey, TilePlan> computeGrouped(
        const std::vector<const TileScheme*>& schemes,
        const Camera& camera,
        double viewportWidthPixels,
        double viewportHeightPixels);
};

} // namespace earth_engine
