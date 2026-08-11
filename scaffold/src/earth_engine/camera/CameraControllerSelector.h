#pragma once

#include "controllers/ICameraController.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace earth_engine {

/// 控制器族的持有者与切换器(Skybolt `CameraControllerSelector` 同构)。
///
/// 它回答**「谁在驱动相机」**这一个问题,不回答"怎么驱动"。切换时
/// `onDeactivate` 旧的、`onActivate` 新的,后者从当前位姿对齐自身状态 ⇒ 零跳变。
///
/// 容器用 `vector<pair<name, ptr>>` 而不是 `map`:控制器数量是个位数,顺序即
/// 注册顺序(可复现),而且 `setViewport` 广播要遍历全部——map 的查找优势在这里
/// 不存在,顺序不确定性反而是负担。
class CameraControllerSelector {
public:
    /// 注册一个控制器。**首个注册者自动成为活动控制器**(并收到 `onActivate`),
    /// 避免"构造完还没选就 tick"这种半初始化窗口。
    void add(std::string name, std::unique_ptr<ICameraController> controller);

    /// 切换。名字不存在或已是当前活动者 ⇒ 返回 false 且不产生任何回调。
    bool select(const std::string& name);

    const std::string& activeName() const { return activeName_; }
    ICameraController* active() const { return active_; }

    /// 输入路由的逃生口:把活动控制器按具体类型取出。
    /// **输入不在 `ICameraController` 里**(见该头文件的说明),故调用方需要拿到
    /// 具体类型才能喂事件。返回 nullptr = 当前驱动者不吃这种输入,事件丢弃。
    template <class T>
    T* activeAs() const {
        return dynamic_cast<T*>(active_);
    }

    /// 按具体类型查找**任意**注册的控制器(不限活动)。装配期接线用。
    template <class T>
    T* findOfType() const {
        for (const auto& entry : controllers_) {
            if (auto* typed = dynamic_cast<T*>(entry.second.get())) {
                return typed;
            }
        }
        return nullptr;
    }

    /// 视口广播:未激活的控制器也要保持正确,否则接管瞬间用错像素→角度增益。
    void setViewport(int widthPixels, int heightPixels);

private:
    std::vector<std::pair<std::string, std::unique_ptr<ICameraController>>>
        controllers_;
    std::string activeName_;
    ICameraController* active_ = nullptr;
};

} // namespace earth_engine
