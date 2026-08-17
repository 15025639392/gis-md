#include <gtest/gtest.h>

#include "earth_engine/core/async/WorkLedger.h"

#include <thread>
#include <vector>

using earth_engine::WorkLedger;
using Kind = earth_engine::WorkLedger::Kind;

namespace {

class WorkLedgerTest : public ::testing::Test {
protected:
    void SetUp() override { WorkLedger::shared().resetForTesting(); }
    void TearDown() override { WorkLedger::shared().resetForTesting(); }
};

// 令牌的全部价值在于"每次 acquire 恰好一次 release"。这条不成立时的症状是
// 永不入睡(泄漏)或画面冻住(提前释放),两者在任何截图里都看不出来 —— 所以
// 归零必须被单测钉死,不能靠 code review。
TEST_F(WorkLedgerTest, AcquireReleaseReturnsToZero) {
    WorkLedger& ledger = WorkLedger::shared();
    EXPECT_EQ(ledger.outstanding(Kind::Landing), 0);
    {
        WorkLedger::Ticket t = ledger.acquire(Kind::Landing, "tile");
        EXPECT_TRUE(t.valid());
        EXPECT_EQ(ledger.outstanding(Kind::Landing), 1);
    }
    EXPECT_EQ(ledger.outstanding(Kind::Landing), 0);
}

// 出错路径才是这个 bug 家族的高发区:上一轮那个"整屏空白零报错"正是取消路径
// 上没人落终态。三条路都必须归零。
TEST_F(WorkLedgerTest, AllThreeExitPathsRelease) {
    WorkLedger& ledger = WorkLedger::shared();
    for (int path = 0; path < 3; ++path) {
        WorkLedger::Ticket t = ledger.acquire(Kind::Landing, "req");
        EXPECT_EQ(ledger.outstanding(Kind::Landing), 1);
        if (path == 0) {
            // 成功:显式释放
            t.release();
        } else if (path == 1) {
            // 失败:提前返回,靠析构
        } else {
            // 取消:显式释放两次(幂等)
            t.release();
            t.release();
        }
        // path==1 的 t 在此处析构
        if (path != 1) {
            EXPECT_EQ(ledger.outstanding(Kind::Landing), 0) << "path=" << path;
        }
    }
    EXPECT_EQ(ledger.outstanding(Kind::Landing), 0);
}

// move 后源令牌必须失效 —— 否则一次 acquire 会被 release 两次,计数变负,
// 判据从此永远说"空闲",退化成没有 gating 之前的冻屏。
TEST_F(WorkLedgerTest, MoveDoesNotDoubleRelease) {
    WorkLedger& ledger = WorkLedger::shared();
    {
        WorkLedger::Ticket a = ledger.acquire(Kind::Pumped, "upload");
        WorkLedger::Ticket b = std::move(a);
        EXPECT_FALSE(a.valid());
        EXPECT_TRUE(b.valid());
        EXPECT_EQ(ledger.outstanding(Kind::Pumped), 1);
    }
    EXPECT_EQ(ledger.outstanding(Kind::Pumped), 0);
}

// Landing 与 Pumped 的帧语义相反,混用就等于没做这个设计:
// Landing 持有期不要求出帧,释放时要一帧;Pumped 持有期必须出帧。
TEST_F(WorkLedgerTest, LandingSignalsOnReleasePumpedSignalsWhileHeld) {
    WorkLedger& ledger = WorkLedger::shared();
    const char* label = nullptr;

    WorkLedger::Ticket landing = ledger.acquire(Kind::Landing, "net");
    EXPECT_FALSE(ledger.hasPumpedWork(&label));   // 持有期不驱动帧
    EXPECT_FALSE(ledger.consumeLanded(&label));   // 还没落地
    landing.release();
    EXPECT_TRUE(ledger.consumeLanded(&label));    // 落地 → 要一帧
    EXPECT_STREQ(label, "net");

    WorkLedger::Ticket pumped = ledger.acquire(Kind::Pumped, "pageUpload");
    EXPECT_TRUE(ledger.hasPumpedWork(&label));    // 持有期必须出帧
    EXPECT_STREQ(label, "pageUpload");
    pumped.release();
    EXPECT_FALSE(ledger.hasPumpedWork(&label));
}

// 落地脏位必须**消费一次**。不消费的话一次到货会让渲染循环永远跑下去 ——
// 这正是 Engine::requestRender 那个 exchange 语义踩过的坑,别再踩第二遍。
TEST_F(WorkLedgerTest, LandedFlagIsConsumedOnce) {
    WorkLedger& ledger = WorkLedger::shared();
    ledger.acquire(Kind::Landing, "a").release();
    EXPECT_TRUE(ledger.consumeLanded(nullptr));
    EXPECT_FALSE(ledger.consumeLanded(nullptr));
}

// 泄漏时要能点名。只报"还有 3 个在途"而不说是谁,现场没法查。
TEST_F(WorkLedgerTest, OutstandingByLabelNamesTheHolder) {
    WorkLedger& ledger = WorkLedger::shared();
    WorkLedger::Ticket a = ledger.acquire(Kind::Landing, "terrainTile");
    WorkLedger::Ticket b = ledger.acquire(Kind::Landing, "terrainTile");
    WorkLedger::Ticket c = ledger.acquire(Kind::Pumped, "pageUpload");
    const auto rows = ledger.outstandingByLabel();
    ASSERT_EQ(rows.size(), 2u);
    int terrain = 0, page = 0;
    for (const auto& [label, count] : rows) {
        if (label == "terrainTile") terrain = count;
        if (label == "pageUpload") page = count;
    }
    EXPECT_EQ(terrain, 2);
    EXPECT_EQ(page, 1);
}

// 释放发生在 worker/网络线程,查询发生在渲染线程。计数错乱的症状同样是
// 静默的,所以并发路径也要钉一下。
TEST_F(WorkLedgerTest, ConcurrentAcquireReleaseIsBalanced) {
    WorkLedger& ledger = WorkLedger::shared();
    constexpr int kThreads = 8;
    constexpr int kPerThread = 500;
    std::vector<std::thread> threads;
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&ledger] {
            for (int n = 0; n < kPerThread; ++n) {
                WorkLedger::Ticket t = ledger.acquire(Kind::Landing, "race");
                (void)t;
            }
        });
    }
    for (auto& t : threads) t.join();
    EXPECT_EQ(ledger.outstanding(Kind::Landing), 0);
    EXPECT_TRUE(ledger.outstandingByLabel().empty());
}

// ── WorkTicketSlot:每帧从 busy 谓词对账的幂等槽 ──
// 新异步源(矢量链、raster overlay)用它把在途状态喂进账本。它的价值就在于
// "每次 reconcile 两个方向都处理" —— 手写配对 acquire/release 的漏放风险被消除。
// 这几条钉死的正是 Phase A 最怕的 bug:reconcile 逻辑写错 → Phase B 翻转后
// 永不入睡(泄漏)或冻屏(误放)。

using earth_engine::WorkTicketSlot;

// active 上升沿 acquire、持续 active 幂等不重复计数、下降沿 release。
TEST_F(WorkLedgerTest, SlotReconcileTracksPredicate) {
    WorkLedger& ledger = WorkLedger::shared();
    WorkTicketSlot slot;
    EXPECT_FALSE(slot.held());

    slot.reconcile(Kind::Landing, "vec", true);          // 上升沿
    EXPECT_TRUE(slot.held());
    EXPECT_EQ(ledger.outstanding(Kind::Landing), 1);

    slot.reconcile(Kind::Landing, "vec", true);          // 持续忙:幂等
    EXPECT_EQ(ledger.outstanding(Kind::Landing), 1);

    slot.reconcile(Kind::Landing, "vec", false);         // 下降沿
    EXPECT_FALSE(slot.held());
    EXPECT_EQ(ledger.outstanding(Kind::Landing), 0);

    slot.reconcile(Kind::Landing, "vec", false);         // 持续闲:幂等
    EXPECT_EQ(ledger.outstanding(Kind::Landing), 0);
}

// 反复 busy/idle 抖动不得泄漏 —— 这是"每帧对账"最常见的输入形态。
TEST_F(WorkLedgerTest, SlotToggleDoesNotLeak) {
    WorkLedger& ledger = WorkLedger::shared();
    WorkTicketSlot slot;
    for (int i = 0; i < 100; ++i) {
        slot.reconcile(Kind::Pumped, "upload", i % 2 == 0);
    }
    slot.reconcile(Kind::Pumped, "upload", false);       // 收尾到 idle
    EXPECT_EQ(ledger.outstanding(Kind::Pumped), 0);
}

// slot 析构(源被销毁)必须释放它持有的令牌,否则源没了令牌还在 = 永不入睡。
TEST_F(WorkLedgerTest, SlotDestructionReleases) {
    WorkLedger& ledger = WorkLedger::shared();
    {
        WorkTicketSlot slot;
        slot.reconcile(Kind::Landing, "overlay", true);
        EXPECT_EQ(ledger.outstanding(Kind::Landing), 1);
    }
    EXPECT_EQ(ledger.outstanding(Kind::Landing), 0);
}

// ── 平台级唤醒回调(Phase B §0)──
// Landing 释放=产物落地=必须踹醒睡着的循环。这条唤醒是"睡下去还能醒过来"的
// 唯一保证;漏触发 = 永久冻屏。故 Landing 释放必触发、Pumped 释放不触发、
// 清除后不再触发,三条都要钉死。
TEST_F(WorkLedgerTest, WakeCallbackFiresOnLandingReleaseOnly) {
    WorkLedger& ledger = WorkLedger::shared();
    int wakes = 0;
    ledger.setWakeCallback([&wakes] { ++wakes; });

    ledger.acquire(Kind::Landing, "net").release();   // Landing 落地 → 唤醒
    EXPECT_EQ(wakes, 1);

    {
        WorkLedger::Ticket p = ledger.acquire(Kind::Pumped, "upload");
        // Pumped 持有期靠 hasPumpedWork 持续出帧,释放不是"落地",不唤醒。
    }
    EXPECT_EQ(wakes, 1);

    ledger.acquire(Kind::Landing, "decode").release();
    EXPECT_EQ(wakes, 2);

    ledger.setWakeCallback(nullptr);                  // 清除(宿主拆除)
    ledger.acquire(Kind::Landing, "late").release();
    EXPECT_EQ(wakes, 2) << "清除后不得再触发 —— 否则回调进已亡宿主";
}

} // namespace
