#pragma once

#include "TilePlan.h"
#include "TileGroupKey.h"
#include <memory>
#include <vector>
#include <unordered_map>

namespace earth_engine {

class TileScheme;
class Camera;

/// 每图层瓦片计划。
/// 由 BasemapLayerStack 计算后分配给各图层。
/// 同 TileGroupKey 的图层共享同一个底层的 TilePlan 数据。
class LayerTilePlan {
public:
    LayerTilePlan() = default;

    /// 从独立计算创建（不同 scheme 的图层）
    static LayerTilePlan independent(TilePlan plan);

    /// 从共享 TilePlan 派生（同 scheme 组内的图层）
    /// @param shared 共享的 TilePlan（生命周期由 stack 管理）
    static LayerTilePlan sharedFrom(const TilePlan* shared);

    /// 是否有效（包含可见瓦片）
    bool valid() const { return plan_ != nullptr; }

    /// 目标 zoom 层级
    int zoom() const { return plan_ ? plan_->zoom : 0; }

    /// 当前帧需要的瓦片键列表
    const std::vector<TileKey>& visibleTiles() const {
        static const std::vector<TileKey> empty;
        return plan_ ? plan_->visibleTiles : empty;
    }

    /// 上一 zoom 层级（parent fallback 用）
    const std::vector<TileKey>& parentTiles() const {
        static const std::vector<TileKey> empty;
        return plan_ ? plan_->parentTiles : empty;
    }

    /// 分组键（标识此 plan 属于哪个 group）
    const TileGroupKey& groupKey() const { return groupKey_; }
    void setGroupKey(const TileGroupKey& key) { groupKey_ = key; }

private:
    const TilePlan* plan_ = nullptr;       // 指向共享或独立拥有的数据
    std::unique_ptr<TilePlan> owned_;      // 独立拥有时持有
    TileGroupKey groupKey_;
};

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
