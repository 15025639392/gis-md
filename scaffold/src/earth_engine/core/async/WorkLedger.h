#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace earth_engine {

/// 异步在途的**单一账本**。
///
/// 存在理由:此前"还有活没干完吗"是由 Scene::hasConvergingWork 在外面**重新
/// 推导**的 —— 它去问四个各自为政的判据(tileset pendingRequests / overlay
/// hasPendingWork / pageStore hasWorkInFlight / 相机自演进)。那四个都不是为
/// 活性信号设计的,其中两个已经因此被修过(页在途判据没有终止态、pan 惯性永不
/// 归零)。判据的正确性 = 四者同时正确,漏一个的症状是**画面冻住且零报错**。
///
/// 令牌化把失效方向倒过来:
///   • 忘记取令牌   → 仍会冻屏(与今天相同,不更差)
///   • 取了忘记释放 → **永不入睡**,且 awake reason 会一直报出这个 label
/// RAII 让"取了不放"成为默认,"提前放"必须显式写代码 —— 危险方向从默认变成
/// 需要故意为之。
///
/// **两种令牌不可混用**(这条区分是整个设计的要害):
///   • Landing —— 在别的线程上自己会走完(网络请求 / worker 镶嵌 / 解码)。
///     持有期间**不需要出帧**;**释放**时才需要一帧去消费落地的产物。
///   • Pumped  —— 必须在渲染帧里被推进(GPU 上传 drain 之类)。停帧 = 永远
///     推不动,所以持有期间**必须持续出帧**。
/// 今天的判据把两类一视同仁当 Pumped,于是一个在飞的网络请求就能把满帧率
/// 按住它的整个生命周期。
///
/// 线程安全:计数走原子,可在任意线程 acquire/release。label 表带锁,只在
/// acquire/release 时碰 —— 相对它记录的那些操作(一次网络请求、一次瓦片上传)
/// 完全是噪声级成本,故**不做 debug-only**:现场诊断需要它在 release 构建里
/// 也能说出"是谁没放手"。
class WorkLedger {
public:
    enum class Kind {
        Landing,
        Pumped,
    };

    /// move-only RAII 令牌。析构即释放;release() 幂等,可显式提前释放。
    class Ticket {
    public:
        Ticket() = default;
        Ticket(const Ticket&) = delete;
        Ticket& operator=(const Ticket&) = delete;
        Ticket(Ticket&& other) noexcept { moveFrom(other); }
        Ticket& operator=(Ticket&& other) noexcept {
            if (this != &other) {
                release();
                moveFrom(other);
            }
            return *this;
        }
        ~Ticket() { release(); }

        void release();
        bool valid() const { return ledger_ != nullptr; }
        Kind kind() const { return kind_; }
        const char* label() const { return label_; }

    private:
        friend class WorkLedger;
        Ticket(WorkLedger* ledger, Kind kind, const char* label)
            : ledger_(ledger), kind_(kind), label_(label) {}
        void moveFrom(Ticket& other) {
            ledger_ = other.ledger_;
            kind_ = other.kind_;
            label_ = other.label_;
            other.ledger_ = nullptr;
        }

        WorkLedger* ledger_ = nullptr;
        Kind kind_ = Kind::Landing;
        const char* label_ = nullptr;
    };

    static WorkLedger& shared();

    /// label 必须是**静态存活**的字符串(字符串字面量):落地脏位只存指针,
    /// 它要活到诊断行打出来那一刻,而那可能远晚于取令牌的调用栈。
    Ticket acquire(Kind kind, const char* label);

    /// Pumped 有在途 = 本帧必须继续出帧。outLabel 取其中一个(诊断用)。
    bool hasPumpedWork(const char** outLabel) const;

    /// 自上次调用以来有 Landing 释放过 = 有产物落地、需要一帧去消费。
    /// **消费一次**(exchange 语义):不消费的话一次落地会让循环永远跑下去。
    bool consumeLanded(const char** outLabel);

    /// Whether a Landing release is waiting to be consumed by the next frame.
    /// This is a non-consuming snapshot for diagnostics/audits.
    bool hasUnconsumedLanding() const;

    /// 任意种类有在途。并行验证期用它与 hasConvergingWork 对拍 —— 那条判据
    /// 表达的是"有没有在途",不区分两类。
    bool anyOutstanding(const char** outLabel) const;

    int outstanding(Kind kind) const;

    /// 单个 label 的在途数。审计用:把令牌账与"从权威容器重新数一遍"的真值
    /// 对拍 —— 漏接一个状态迁移点的症状否则是静默的。
    int outstandingForLabel(const char* label) const;

    /// 诊断:当前未释放令牌的 label → 计数。泄漏时用它点名。
    std::vector<std::pair<std::string, int>> outstandingByLabel() const;

    /// 设置**唤醒回调**(Phase B 平台级唤醒,§0):每次 Landing 令牌**释放**时
    /// 触发一次 —— 有产物落地了,睡着的渲染循环需要被踹醒去消费。传 nullptr 清除
    /// (宿主/引擎拆除时必须清,否则 worker 线程回调进已亡对象)。
    ///
    /// 线程安全:回调可能在任意 worker/网络线程被触发;set/clear 与触发用独立锁,
    /// 触发时先在锁下取副本再在锁外调用(回调里通常做 ALooper_wake 之类,不应
    /// 持本账本的锁)。回调本身必须线程安全且可重入。
    void setWakeCallback(std::function<void()> cb);

    /// 单测用:清空全部状态。生产路径不得调用。
    void resetForTesting();

private:
    // 嵌套类天然可访问外层私有成员(C++11 起),无需 friend 声明。
    void release(Kind kind, const char* label);

    std::atomic<int> landing_{0};
    std::atomic<int> pumped_{0};
    /// Landing 释放过的一次性脏位 + 最后一个释放者的 label。
    std::atomic<bool> landed_{false};
    std::atomic<const char*> lastLandedLabel_{nullptr};

    /// 唤醒回调(见 setWakeCallback)。独立锁:触发在 release 路径(可能已在
    /// 其他锁语境下),不与 labelMutex_ 共用免得放大锁竞争/引入锁序。
    mutable std::mutex wakeMutex_;
    std::function<void()> wakeCallback_;

    mutable std::mutex labelMutex_;
    /// 按 **string 值**而不是 const char* 指针索引:同一份字面量在不同翻译
    /// 单元里未必是同一个指针,按指针索引会让同名 label 在表里裂成好几行。
    /// 分 kind 存:awake reason 要回答的是"**谁**在按着帧率不放",而那只可能
    /// 是 Pumped —— 混在一张表里会报出一个不驱动帧的 Landing label,把人引开。
    std::map<std::string, int> labelCounts_[2];

    static std::size_t slot(Kind kind) {
        return kind == Kind::Landing ? 0u : 1u;
    }
};

/// 一个源"我此刻忙不忙"→ 账本令牌的**幂等对账槽**。
///
/// 把 `if (active && !ticket.valid()) acquire; else if (!active && valid) release;`
/// 这段(`TilesetTile::syncContentLoadWorkTicket` / `TerrainPageStore::syncWorkTicket`
/// 手写过两遍的同款)收成一处:源每帧(或每次状态变化)拿当前 busy 谓词调一次
/// `reconcile`,令牌自动跟随。相比手写配对 acquire/release,漏放的可能被消除
/// —— 每次 reconcile 两个方向都处理,不存在"某条出错路径忘了 release"。
///
/// 线程约定:非线程安全,单个 slot 只能由一个线程(通常是渲染线程,或持有
/// 源自身锁的线程)驱动。label 必须静态存活(见 WorkLedger::acquire)。
class WorkTicketSlot {
public:
    WorkTicketSlot() = default;
    WorkTicketSlot(const WorkTicketSlot&) = delete;
    WorkTicketSlot& operator=(const WorkTicketSlot&) = delete;
    WorkTicketSlot(WorkTicketSlot&&) = delete;
    WorkTicketSlot& operator=(WorkTicketSlot&&) = delete;

    /// active=true 确保持有一张 (kind,label) 令牌;false 确保已释放。幂等。
    /// [2026-08-21 冻屏根修] 加锁:worker 完成/派发路径与渲染帧 reconcile 并发。
    void reconcile(WorkLedger::Kind kind, const char* label, bool active) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (active && !ticket_.valid()) {
            ticket_ = WorkLedger::shared().acquire(kind, label);
        } else if (!active && ticket_.valid()) {
            ticket_.release();
        }
    }

    /// worker 完成路径:最后一件在途落地时释放 → 触发 Landing 落地唤醒。
    /// 幂等(未持有则 no-op),线程安全。
    void releaseFromAnyThread() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (ticket_.valid()) {
            ticket_.release();
        }
    }

    /// worker 派发新在途工作时确保持有(睡死期间 worker 起的新活不能无票)。
    /// 幂等(已持有则 no-op),线程安全。
    void ensureHeld(WorkLedger::Kind kind, const char* label) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!ticket_.valid()) {
            ticket_ = WorkLedger::shared().acquire(kind, label);
        }
    }

    bool held() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return ticket_.valid();
    }

private:
    mutable std::mutex mutex_;
    WorkLedger::Ticket ticket_;
};

} // namespace earth_engine
