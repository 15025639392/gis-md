#include "SelectionManager.h"
#include "PickingService.h"

namespace earth_engine {

void SelectionManager::setFeatureState(const std::string& layerId,
                                        const std::string& featureId,
                                        FeatureState state) {
    if (onStateChanged_) {
        onStateChanged_(layerId, featureId, state);
    }
}

// ---- Hover ----

void SelectionManager::onHover(const PickResult& pickResult) {
    FeatureRef incoming;
    if (pickResult.hitType == PickResult::HitType::VectorFeature) {
        incoming.layerId = pickResult.layerId;
        incoming.featureId = pickResult.featureId;
    }

    // 相同 feature 不变
    if (incoming == hovered_) return;

    // 清除旧 hover
    if (!hovered_.empty()) {
        setFeatureState(hovered_.layerId, hovered_.featureId,
                        FeatureState::None);
    }

    // 设置新 hover（跳过已在 selected 中的 feature）
    if (!incoming.empty()) {
        bool alreadySelected = (selected_.count(incoming) > 0);
        setFeatureState(incoming.layerId, incoming.featureId,
                        alreadySelected ? FeatureState::Selected
                                        : FeatureState::Hovered);
    }

    hovered_ = std::move(incoming);
}

void SelectionManager::clearHover() {
    onHover(PickResult{});
}

// ---- 选择 ----

void SelectionManager::onSelect(const PickResult& pickResult) {
    // 清除旧选择
    clearSelection();

    if (pickResult.hitType == PickResult::HitType::VectorFeature) {
        addToSelection(pickResult.layerId, pickResult.featureId);
    }
}

void SelectionManager::onSelectAdd(const PickResult& pickResult) {
    if (pickResult.hitType == PickResult::HitType::VectorFeature) {
        FeatureRef ref{pickResult.layerId, pickResult.featureId};
        if (selected_.count(ref) == 0) {
            addToSelection(ref.layerId, ref.featureId);
        }
    }
}

void SelectionManager::onSelectToggle(const PickResult& pickResult) {
    if (pickResult.hitType != PickResult::HitType::VectorFeature) return;

    FeatureRef ref{pickResult.layerId, pickResult.featureId};
    if (selected_.count(ref) > 0) {
        removeFromSelection(ref.layerId, ref.featureId);
    } else {
        addToSelection(ref.layerId, ref.featureId);
    }
}

void SelectionManager::clearSelection() {
    for (const auto& ref : selected_) {
        setFeatureState(ref.layerId, ref.featureId, FeatureState::None);
    }
    selected_.clear();
    cachedFirstSelected_ = FeatureRef{};
    selectedDirty_ = true;
}

const FeatureRef& SelectionManager::firstSelected() const {
    if (selectedDirty_ && !selected_.empty()) {
        cachedFirstSelected_ = *selected_.begin();
        selectedDirty_ = false;
    }
    return cachedFirstSelected_;
}

// ---- 内部 ----

void SelectionManager::addToSelection(const std::string& layerId,
                                       const std::string& featureId) {
    FeatureRef ref{layerId, featureId};
    auto [it, inserted] = selected_.insert(ref);
    if (inserted) {
        selectedDirty_ = true;
    }
    setFeatureState(layerId, featureId, FeatureState::Selected);

    // 如果新选中项也是当前 hover，更新 hover 显示为 selected
    if (ref == hovered_) {
        // hover callback 下次 onHover 时自然处理
    }
}

void SelectionManager::removeFromSelection(const std::string& layerId,
                                            const std::string& featureId) {
    FeatureRef ref{layerId, featureId};
    selected_.erase(ref);
    selectedDirty_ = true;
    setFeatureState(layerId, featureId, FeatureState::None);
}

} // namespace earth_engine
