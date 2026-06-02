#pragma once

#include <string>
#include <vector>
#include <functional>
#include <unordered_set>

namespace earth_engine {

struct PickResult;

/// Feature 交互状态（用于 VectorLayer 样式响应）。
enum class FeatureState {
    None,
    Hovered,
    Selected
};

/// Feature 标识（layerId + featureId 对）。
struct FeatureRef {
    std::string layerId;
    std::string featureId;

    bool operator==(const FeatureRef& o) const {
        return layerId == o.layerId && featureId == o.featureId;
    }

    bool empty() const { return layerId.empty() && featureId.empty(); }
};

} // namespace earth_engine

// std::hash 用于 unordered_set
namespace std {
    template<> struct hash<earth_engine::FeatureRef> {
        size_t operator()(const earth_engine::FeatureRef& r) const {
            size_t h1 = hash<string>{}(r.layerId);
            size_t h2 = hash<string>{}(r.featureId);
            return h1 ^ (h2 << 1);
        }
    };
}

namespace earth_engine {

/// 选择状态管理器。
///
/// 集中管理 hovered / selected features，支持多选（Shift 加选、Ctrl/Meta 切换）。
/// 通过回调通知 VectorLayer 更新交互样式。
/// 不持有 GPU 资源，不重建 geometry。
class SelectionManager {
public:
    /// 状态变化回调类型
    /// @param layerId 图层 ID
    /// @param featureId feature ID
    /// @param state 新状态
    using StateChangeCallback = std::function<void(
        const std::string& layerId,
        const std::string& featureId,
        FeatureState state)>;

    SelectionManager() = default;

    /// 注册状态变化回调
    void setStateChangeCallback(StateChangeCallback cb) {
        onStateChanged_ = std::move(cb);
    }

    // ---- Hover ----

    /// 处理悬停变化
    void onHover(const PickResult& pickResult);

    /// 清除 hover
    void clearHover();

    /// 当前 hover
    const FeatureRef& hovered() const { return hovered_; }

    // ---- 选择 ----

    /// 单选（替换当前选择集）
    void onSelect(const PickResult& pickResult);

    /// 加选（Shift — 保留现有选择，追加新选）
    void onSelectAdd(const PickResult& pickResult);

    /// 切换选择（Ctrl/Meta — 已选则取消，未选则添加）
    void onSelectToggle(const PickResult& pickResult);

    /// 清除所有选择
    void clearSelection();

    /// 当前选中集合
    const std::unordered_set<FeatureRef>& selected() const { return selected_; }

    /// 首个选中项（用于单选兼容）
    const FeatureRef& firstSelected() const;

    /// 选中数量
    size_t selectedCount() const { return selected_.size(); }

private:
    void setFeatureState(const std::string& layerId,
                         const std::string& featureId,
                         FeatureState state);

    void addToSelection(const std::string& layerId,
                        const std::string& featureId);

    void removeFromSelection(const std::string& layerId,
                             const std::string& featureId);

    StateChangeCallback onStateChanged_;

    FeatureRef hovered_;
    std::unordered_set<FeatureRef> selected_;
    mutable FeatureRef cachedFirstSelected_;  // 加速 firstSelected()
    mutable bool selectedDirty_ = true;
};

} // namespace earth_engine
