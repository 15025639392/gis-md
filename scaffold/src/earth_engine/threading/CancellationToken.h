#pragma once

#include <memory>
#include <atomic>

namespace earth_engine {

/// 取消令牌。
/// 每个 TilePlan 帧生成一个新令牌。相机移动后旧令牌被 cancel，
/// 所有关联的线程池任务在检查点后丢弃结果。
///
/// 线程安全：isCancelled() 可从任意线程调用。
class CancellationToken {
public:
    CancellationToken() : cancelled_(std::make_shared<std::atomic<bool>>(false)) {}

    bool isCancelled() const { return cancelled_->load(std::memory_order_acquire); }

    void cancel() { cancelled_->store(true, std::memory_order_release); }

    /// 创建子令牌（父取消则子也取消）
    CancellationToken createChild() const {
        CancellationToken child;
        child.cancelled_ = cancelled_;  // 共享同一个原子变量
        return child;
    }

private:
    std::shared_ptr<std::atomic<bool>> cancelled_;
};

} // namespace earth_engine
