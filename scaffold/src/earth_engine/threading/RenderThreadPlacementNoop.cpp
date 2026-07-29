#include "earth_engine/threading/RenderThreadPlacement.h"

// 非 Android 平台的空实现:接口在所有平台都可编译可调用,宿主不必写平台分支。
//
// 为什么其它平台暂时不需要:Apple 端的渲染循环由系统框架(CADisplayLink /
// CVDisplayLink)驱动,线程对系统不是匿名的,没有 Android 这套"裸线程被 EAS 扔进
// 小核簇"的问题。将来若要接 QoS(pthread_set_qos_class_self_np),在这里补即可 ——
// 头文件契约不变。
namespace earth_engine {

struct RenderThreadPlacement::Impl {};

RenderThreadPlacement::RenderThreadPlacement() = default;
RenderThreadPlacement::~RenderThreadPlacement() = default;

RenderThreadPlacement::Status RenderThreadPlacement::applyToCurrentThread(
    const Options& options) {
    (void)options;
    return Status{};
}

void RenderThreadPlacement::reportActualWorkDurationMs(double actualMs) {
    (void)actualMs;
}

void RenderThreadPlacement::setTargetFrameMs(double targetFrameMs) {
    (void)targetFrameMs;
}

void RenderThreadPlacement::release() {}

RenderThreadPlacement::Status RenderThreadPlacement::status() const {
    return Status{};
}

}  // namespace earth_engine
