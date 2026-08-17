#include "WorkLedger.h"

namespace earth_engine {

WorkLedger& WorkLedger::shared() {
    static WorkLedger instance;
    return instance;
}

WorkLedger::Ticket WorkLedger::acquire(Kind kind, const char* label) {
    if (label == nullptr) {
        label = "(unlabeled)";
    }
    if (kind == Kind::Landing) {
        landing_.fetch_add(1, std::memory_order_acq_rel);
    } else {
        pumped_.fetch_add(1, std::memory_order_acq_rel);
    }
    {
        std::lock_guard<std::mutex> lock(labelMutex_);
        ++labelCounts_[slot(kind)][label];
    }
    return Ticket(this, kind, label);
}

void WorkLedger::release(Kind kind, const char* label) {
    bool landed = false;
    if (kind == Kind::Landing) {
        landing_.fetch_sub(1, std::memory_order_acq_rel);
        // 落地脏位:Landing 的价值全在**释放**这一刻 —— 有东西到货了,需要
        // 一帧去消费它。持有期间反而不需要出帧(它在别的线程上跑)。
        lastLandedLabel_.store(label, std::memory_order_relaxed);
        landed_.store(true, std::memory_order_release);
        landed = true;
    } else {
        pumped_.fetch_sub(1, std::memory_order_acq_rel);
    }
    {
        std::lock_guard<std::mutex> lock(labelMutex_);
        auto& counts = labelCounts_[slot(kind)];
        auto it = counts.find(label);
        if (it != counts.end() && --it->second <= 0) {
            counts.erase(it);
        }
    }
    // 平台级唤醒(§0):产物落地了,踹醒睡着的渲染循环去消费。取副本再在锁外
    // 调用 —— 回调里通常做 ALooper_wake,不应持本账本任何锁。
    if (landed) {
        std::function<void()> cb;
        {
            std::lock_guard<std::mutex> lock(wakeMutex_);
            cb = wakeCallback_;
        }
        if (cb) cb();
    }
}

void WorkLedger::setWakeCallback(std::function<void()> cb) {
    std::lock_guard<std::mutex> lock(wakeMutex_);
    wakeCallback_ = std::move(cb);
}

void WorkLedger::Ticket::release() {
    if (ledger_ == nullptr) {
        return;
    }
    WorkLedger* ledger = ledger_;
    ledger_ = nullptr;  // 先清再调:release 幂等,且不给重入留缝
    ledger->release(kind_, label_);
}

bool WorkLedger::hasPumpedWork(const char** outLabel) const {
    if (pumped_.load(std::memory_order_acquire) <= 0) {
        return false;
    }
    if (outLabel) {
        *outLabel = "pumped";
        std::lock_guard<std::mutex> lock(labelMutex_);
        for (const auto& entry : labelCounts_[slot(Kind::Pumped)]) {
            if (entry.second > 0) {
                *outLabel = entry.first.c_str();
                break;
            }
        }
    }
    return true;
}

bool WorkLedger::consumeLanded(const char** outLabel) {
    if (!landed_.exchange(false, std::memory_order_acq_rel)) {
        return false;
    }
    if (outLabel) {
        const char* label = lastLandedLabel_.load(std::memory_order_relaxed);
        *outLabel = label ? label : "landed";
    }
    return true;
}

bool WorkLedger::anyOutstanding(const char** outLabel) const {
    const bool busy = landing_.load(std::memory_order_acquire) > 0 ||
                      pumped_.load(std::memory_order_acquire) > 0;
    if (!busy) {
        return false;
    }
    if (outLabel) {
        *outLabel = "outstanding";
    }
    return true;
}

int WorkLedger::outstandingForLabel(const char* label) const {
    if (label == nullptr) return 0;
    std::lock_guard<std::mutex> lock(labelMutex_);
    int total = 0;
    for (std::size_t s = 0; s < 2; ++s) {
        auto it = labelCounts_[s].find(label);
        if (it != labelCounts_[s].end()) total += it->second;
    }
    return total;
}

int WorkLedger::outstanding(Kind kind) const {
    return kind == Kind::Landing ? landing_.load(std::memory_order_acquire)
                                 : pumped_.load(std::memory_order_acquire);
}

std::vector<std::pair<std::string, int>> WorkLedger::outstandingByLabel()
    const {
    std::vector<std::pair<std::string, int>> out;
    std::lock_guard<std::mutex> lock(labelMutex_);
    for (std::size_t s = 0; s < 2; ++s) {
        for (const auto& entry : labelCounts_[s]) {
            if (entry.second > 0) {
                out.emplace_back(entry.first, entry.second);
            }
        }
    }
    return out;
}

void WorkLedger::resetForTesting() {
    landing_.store(0, std::memory_order_release);
    pumped_.store(0, std::memory_order_release);
    landed_.store(false, std::memory_order_release);
    lastLandedLabel_.store(nullptr, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(labelMutex_);
    labelCounts_[0].clear();
    labelCounts_[1].clear();
}

} // namespace earth_engine
