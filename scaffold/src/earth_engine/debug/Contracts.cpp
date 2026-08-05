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

const char* const kNames[kCount] = {
    "DemNodataSentinel",
    "TexcoordNwOrigin",
};

}  // namespace

const char* name(Id id) {
    const size_t index = static_cast<size_t>(id);
    return index < kCount ? kNames[index] : "?";
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

uint32_t totalViolations(Id id) {
    const size_t index = static_cast<size_t>(id);
    return index < kCount
        ? totalCounts_[index].load(std::memory_order_relaxed)
        : 0u;
}

}  // namespace contracts
}  // namespace earth_engine
