#pragma once

#include "BasemapLayer.h"
#include "../tiling/TileGroupKey.h"
#include "../tiling/TilePlan.h"
#include "../renderer/RenderCommand.h"
#include "../core/math/Rectangle.h"

#include <memory>
#include <vector>
#include <string>
#include <unordered_map>

namespace earth_engine {

class Renderer;
struct FrameState;
class Camera;
class TileScheme;

/// 底图图层栈管理器。
///
/// 职责：
///   1. 管理 BasemapLayer 的添加/移除/排序
///   2. 按 TileGroupKey 分组，共享 TilePlan 计算
///   3. 统一驱动图层更新和渲染命令生成
///   4. 控制点验证
///
/// 分组规则：
///   同 scheme + 同 zoom + 同 viewport 的图层共享一次 TilePlan 计算。
///   各图层保持独立的纹理缓存（TileTextureCache）。
class BasemapLayerStack {
public:
    BasemapLayerStack() = default;
    ~BasemapLayerStack() = default;

    BasemapLayerStack(const BasemapLayerStack&) = delete;
    BasemapLayerStack& operator=(const BasemapLayerStack&) = delete;

    // ---- 图层管理 ----

    /// 添加底图图层（栈顶，最后渲染）
    void addLayer(std::unique_ptr<BasemapLayer> layer);

    /// 移除图层
    /// @return 被移除的图层（所有权返回调用者），未找到返回 nullptr
    std::unique_ptr<BasemapLayer> removeLayer(const std::string& layerId);

    /// 将图层移动到指定位置
    /// @param layerId 图层 ID
    /// @param index 目标位置（0 = 栈底，最先渲染）
    void moveLayer(const std::string& layerId, size_t index);

    /// 获取图层数量
    size_t layerCount() const { return layers_.size(); }

    int visibleTileCount() const;
    int cachedTileCount() const;

    /// 获取图层（非空）
    BasemapLayer* layerAt(size_t index);

    /// 按 ID 查找
    BasemapLayer* findLayer(const std::string& layerId);

    // ---- 帧更新 ----

    /// 每帧更新所有可见图层
    /// 1. 按 TileGroupKey 分组
    /// 2. 每组计算一次 TilePlan
    /// 3. 分发 TilePlan 给各图层
    /// 4. 各图层加载缺失瓦片
    void update(const FrameState& frameState);

    // ---- 渲染命令 ----

    /// 按图层顺序生成渲染命令
    void buildRenderCommands(Renderer& renderer,
                             RenderCommandList& commands);

    // ---- 调试 ----

    /// 获取所有图层的可见瓦片（供 DebugOverlay 使用）
    std::vector<std::pair<std::string, std::vector<TileKey>>> allVisibleTiles() const;

    /// 获取当前分组的 TilePlan（调试用）
    const std::unordered_map<TileGroupKey, TilePlan>& groupPlans() const {
        return groupPlans_;
    }

    // ---- 控制点验证 ----

    /// 控制点验证结果
    struct ControlPointResult {
        std::string layerId;
        TileKey tileKey;
        Rectangle tileBounds;       // radian
        bool inExpectedRange;
    };

    /// 对指定地理坐标在所有图层间进行控制点验证。
    /// 检查各图层 scheme 下同一坐标的 tile 位置是否存在系统性偏移。
    /// @param lngRad 经度（radian）
    /// @param latRad 纬度（radian）
    /// @param zoom 验证 zoom 层级（默认 10）
    /// @return 每图层的控制点结果
    std::vector<ControlPointResult> verifyControlPoint(
        double lngRad, double latRad, int zoom = 10) const;

    // ---- 访问器 ----

    /// 所有图层（按渲染顺序，index 0 先渲染）
    const std::vector<std::unique_ptr<BasemapLayer>>& layers() const {
        return layers_;
    }

private:
    std::vector<std::unique_ptr<BasemapLayer>> layers_;
    std::unordered_map<TileGroupKey, TilePlan> groupPlans_;
    std::unordered_map<std::string, int> previousZoomByScheme_;
};

} // namespace earth_engine
