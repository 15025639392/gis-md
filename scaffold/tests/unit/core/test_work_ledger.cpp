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

} // namespace
