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

// 每条边的两端。producer 是**通常该改的人**,consumer 只是发现的人。
// 串是逐条 grep 核过的实际写入点/判定点,不是凭印象填的;改动判定点位置时
// 必须同步改这里,否则日志会把人送去错的文件 —— 那比没有归属更糟。
const Owners kOwners[kCount] = {
    // 哨兵在解码时注册(两处 encoding 分支),消费者只是第一个按它采样的人。
    {"HeightmapTerrainProvider::decodeTile",
     "HeightmapTerrainContentProvider::buildContent"},
    // 地形网格的 texcoord 由网格构建器按 NW 公式发;上采样器按同一约定切子窗。
    // (glTF 解析路径 GltfModel.cpp 也产 texcoord,但不喂地形上采样,故不列。)
    {"EllipsoidTerrainMeshBuilder::makeModel",
     "GltfTerrainUpsampler::upsampleForRasterOverlay"},
    // gridN 由 draw 命令构建器写进 heightDisplace[3](两处:位移分支 + remap 分支)。
    {"GltfDrawCommandBuilder::build",
     "TerrainInstanceBatcher::assemble"},
    // ⚠️ 这条两端同属一个模块 —— 它其实不是层间的边,是**模块内的时序不变量**:
    // TerrainPageDecorator 头文件里"页存储保证 tickDecorator 先于本帧任何
    // decoratePage"这句承诺是页存储自己给的,所以违约时该改的也是它。如实登记,
    // 不为了凑成跨模块的样子编一个假的生产方。
    {"TerrainPageStore::tick (时序承诺方)",
     "TerrainPageStore::decoratePage 调用点"},
};

// 三张表与枚举同长 —— 漏一个会让日志报 "?" 或指向错的模块,恰好在出问题时最不该
// 发生。新增 Id 时编译器会在这里点名,不靠 review 记得。
static_assert(sizeof(kNames) / sizeof(kNames[0]) == kCount,
              "contracts::Id 与 kNames 必须逐项对应:新增枚举时补名字。");
static_assert(sizeof(kOwners) / sizeof(kOwners[0]) == kCount,
              "contracts::Id 与 kOwners 必须逐项对应:新增枚举时补两端归属。");

}  // namespace

const char* name(Id id) {
    const size_t index = static_cast<size_t>(id);
    return index < kCount ? kNames[index] : "?";
}

Owners owners(Id id) {
    const size_t index = static_cast<size_t>(id);
    return index < kCount ? kOwners[index] : Owners{"?", "?"};
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
    // producer 排在 consumer 之前:先看到的应该是**该改的人**,而不是恰好发现的
    // 那个模块。只报判定点会把人送去翻错的文件。
    const Owners who = owners(id);
    platformLog(LogLevel::Warning, "Contract",
                "VIOLATED %s producer=%s consumer=%s | %s",
                name(id), who.producer, who.consumer, detail);
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
    // 死边单独点名到判定点所在:dead=2 只说"两条边是黑的",不说黑在谁那儿。
    // 每条一行,因为这行是要被拿去派活的。
    for (size_t i = 0; i < kCount; ++i) {
        if (evalCounts_[i].load(std::memory_order_relaxed) != 0) continue;
        platformLog(LogLevel::Warning, "Contract",
                    "  DEAD %s — 判定点未执行 consumer=%s (该边此刻等于不存在)",
                    kNames[i], kOwners[i].consumer);
    }
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
