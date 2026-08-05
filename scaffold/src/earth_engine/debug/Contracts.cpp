#include "Contracts.h"

#include "PlatformLog.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>

namespace earth_engine {
namespace contracts {
namespace {

constexpr size_t kCount = static_cast<size_t>(Id::Count);

// 三份状态,各自的用途不同:
//   frameCounts_ —— 帧末汇总后清零,区分「暂态违约」(加载中冒一下)与「稳态违约」
//                   (每帧都在违,是真坏了)。
//   totalCounts_ —— 只增不减,给"这次运行到底违过没有"一个终局答案。
//   reported_    —— 首违约详细日志的一次性闸;违约常每帧数千次,全打会淹掉
//                   logcat 并改变时序。
std::atomic<uint32_t> frameCounts_[kCount];
std::atomic<uint32_t> totalCounts_[kCount];
std::atomic<bool> reported_[kCount];
// 求值次数(通过 + 违约)。0 = 判定点从未执行 = 这条契约此刻等于不存在,而只看
// 违约数是看不出这件事的(死契约与永远成立的契约都报 0 违约)。
std::atomic<uint32_t> evalCounts_[kCount];

const char* const kNames[kCount] = {
    "DemNodataSentinel",
    "TexcoordNwOrigin",
    "BatchTemplateGridParity",
    "PageDecorateOrdering",
};

// 名字表与枚举同长 —— 漏一个会让日志报 "?",恰好在出问题时最不该发生。
static_assert(sizeof(kNames) / sizeof(kNames[0]) == kCount,
              "contracts::Id 与 kNames 必须逐项对应:新增枚举时补名字。");

}  // namespace

const char* name(Id id) {
    const size_t index = static_cast<size_t>(id);
    return index < kCount ? kNames[index] : "?";
}

void recordEvaluation(Id id) {
    const size_t index = static_cast<size_t>(id);
    if (index >= kCount) return;
    evalCounts_[index].fetch_add(1, std::memory_order_relaxed);
}

void recordViolation(Id id, const char* fmt, ...) {
    const size_t index = static_cast<size_t>(id);
    if (index >= kCount) return;

    frameCounts_[index].fetch_add(1, std::memory_order_relaxed);
    totalCounts_[index].fetch_add(1, std::memory_order_relaxed);

    // 首违约(按 id,进程内一次)才格式化 —— 把 vsnprintf 挡在热路径外,违约态下
    // 的开销仍是一个原子加。
    if (reported_[index].exchange(true, std::memory_order_relaxed)) {
        return;
    }
    char detail[512];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(detail, sizeof(detail), fmt, args);
    va_end(args);
    platformLog(LogLevel::Warning, "Contract",
                "VIOLATED %s — %s", name(id), detail);
}

void logFrameSummary(uint64_t frameId) {
    char line[256];
    int offset = 0;
    bool any = false;
    for (size_t i = 0; i < kCount; ++i) {
        const uint32_t frame =
            frameCounts_[i].exchange(0, std::memory_order_relaxed);
        if (frame == 0) continue;
        any = true;
        const uint32_t total = totalCounts_[i].load(std::memory_order_relaxed);
        const int written = std::snprintf(
            line + offset, sizeof(line) - static_cast<size_t>(offset),
            "%s%s:%u(tot=%u)", offset > 0 ? " " : "",
            kNames[i], frame, total);
        if (written <= 0 ||
            static_cast<size_t>(offset + written) >= sizeof(line)) {
            break;  // 截断优于溢出;kCount 远小于容量,实际到不了这里
        }
        offset += written;
    }
    // 全绿不打 —— 稳态零日志量,这一行只在出问题时出现。
    if (!any) return;
    platformLog(LogLevel::Warning, "Contract", "f=%llu %s",
                static_cast<unsigned long long>(frameId), line);
}

void logCoverage(uint64_t frameId) {
    char line[512];
    int offset = 0;
    int deadEdges = 0;
    for (size_t i = 0; i < kCount; ++i) {
        const uint32_t evals = evalCounts_[i].load(std::memory_order_relaxed);
        if (evals == 0) ++deadEdges;
        const int written = std::snprintf(
            line + offset, sizeof(line) - static_cast<size_t>(offset),
            "%s%s=%u", offset > 0 ? " " : "", kNames[i], evals);
        if (written <= 0 ||
            static_cast<size_t>(offset + written) >= sizeof(line)) {
            break;
        }
        offset += written;
    }
    // 有 coverage=0 的边就升 Warning:那条契约的判定点没跑到,它此刻等于不存在
    // ——这是需要处理的信号,不是背景噪声。
    platformLog(deadEdges > 0 ? LogLevel::Warning : LogLevel::Info,
                "Contract", "coverage f=%llu dead=%d %s",
                static_cast<unsigned long long>(frameId), deadEdges, line);
}

uint32_t totalEvaluations(Id id) {
    const size_t index = static_cast<size_t>(id);
    return index < kCount
        ? evalCounts_[index].load(std::memory_order_relaxed)
        : 0u;
}

uint32_t totalViolations(Id id) {
    const size_t index = static_cast<size_t>(id);
    return index < kCount
        ? totalCounts_[index].load(std::memory_order_relaxed)
        : 0u;
}

}  // namespace contracts
}  // namespace earth_engine
