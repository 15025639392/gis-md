#include "CameraControllerSelector.h"

namespace earth_engine {

void CameraControllerSelector::add(
    std::string name, std::unique_ptr<ICameraController> controller) {
    if (!controller) {
        return;
    }
    ICameraController* raw = controller.get();
    controllers_.emplace_back(std::move(name), std::move(controller));
    if (!active_) {
        active_ = raw;
        activeName_ = controllers_.back().first;
        active_->onActivate();
    }
}

bool CameraControllerSelector::select(const std::string& name) {
    if (name == activeName_ && active_ != nullptr) {
        return false;
    }
    for (const auto& entry : controllers_) {
        if (entry.first != name) {
            continue;
        }
        if (active_) {
            active_->onDeactivate();
        }
        active_ = entry.second.get();
        activeName_ = entry.first;
        // 接管者从当前相机位姿对齐自身状态 ⇒ 切换零跳变。
        active_->onActivate();
        return true;
    }
    return false;  // 未知名字:保持现状,不留下"没人在驱动"的空档
}

void CameraControllerSelector::setViewport(int widthPixels, int heightPixels) {
    for (const auto& entry : controllers_) {
        entry.second->setViewport(widthPixels, heightPixels);
    }
}

} // namespace earth_engine
