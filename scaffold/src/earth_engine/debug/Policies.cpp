#include "Policies.h"

#include "PlatformLog.h"

#include <atomic>
#include <cstdio>

namespace earth_engine {
namespace policy {
namespace {

constexpr size_t kCount = static_cast<size_t>(Id::Count);

// 窗口内累计。报表输出后清零 —— 不清零的话报的是"自启动以来的平均",冷启动那
// 几百帧会永久拖住数字,而 A/B 里最怕的正是把陈旧值当当前值读。
std::atomic<uint64_t> windowNum_[kCount];
std::atomic<uint64_t> windowDen_[kCount];

const char* const kNames[kCount] = {
    "BatchFormation",
    "PageResidency",
};

const Expectation kExpectations[kCount] = {
    // 合批率。有资格的命令**按定义**就是"能合的",资格闸已经把不能合的挡在外面
    // 了,所以剩下的应当绝大多数进批;掉下来只可能是分组太碎(每组不足 2 个)。
    // 下界取 0.5 而非 0.9:单例组是正常现象(孤立瓦片、边缘 {z,row}),留出余量。
    {0.5, 1.0,
     "有资格命令按定义可合批(资格闸已挡掉不可合的);低于此说明分组过碎 —— "
     "真机拉远相机实测 20/20=1.00,默认相机 2/3。恒 0 = 合批完全空转,"
     "曾因资格闸事实上不可达而长期如此且无人察觉",
     "TerrainInstanceBatcher::assemble + TerrainPageStore 资格闸"},

    // 页驻留率。合批资格 = 该瓦片所有会产生片元的 cell 都有页。稳态下可见瓦片
    // 应当基本都达标;长期偏低说明页 fetch 跟不上或资格闸又变得不可达。
    // 下界 0.5:加载期本来就会低,窗口聚合已摊掉大部分暂态。
    {0.5, 1.0,
     "稳态下可见瓦片应基本都达到全 cell 驻留;偏低 = 页 fetch 跟不上,或资格闸"
     "定义再次变得不可达(旧闸要 gridN²=1024 全驻留而全局仅 ~52 页,恒为 0)",
     "TerrainPageStore::updateVisiblePages"},
};

static_assert(sizeof(kNames) / sizeof(kNames[0]) == kCount,
              "policy::Id 与 kNames 必须逐项对应。");
static_assert(sizeof(kExpectations) / sizeof(kExpectations[0]) == kCount,
              "policy::Id 与 kExpectations 必须逐项对应:新增策略必须给出区间"
              "**和依据**,写不出依据就先别加。");

}  // namespace

const char* name(Id id) {
    const size_t index = static_cast<size_t>(id);
    return index < kCount ? kNames[index] : "?";
}

Expectation expectation(Id id) {
    const size_t index = static_cast<size_t>(id);
    return index < kCount ? kExpectations[index]
                          : Expectation{0.0, 1.0, "?", "?"};
}

void observe(Id id, int numerator, int denominator) {
    const size_t index = static_cast<size_t>(id);
    if (index >= kCount || denominator <= 0) return;  // 无机会不计
    windowNum_[index].fetch_add(static_cast<uint64_t>(numerator < 0 ? 0
                                                                    : numerator),
                                std::memory_order_relaxed);
    windowDen_[index].fetch_add(static_cast<uint64_t>(denominator),
                                std::memory_order_relaxed);
}

uint64_t windowNumerator(Id id) {
    const size_t index = static_cast<size_t>(id);
    return index < kCount ? windowNum_[index].load(std::memory_order_relaxed)
                          : 0;
}

uint64_t windowDenominator(Id id) {
    const size_t index = static_cast<size_t>(id);
    return index < kCount ? windowDen_[index].load(std::memory_order_relaxed)
                          : 0;
}

void logReport(uint64_t frameId) {
    char line[512];
    int offset = 0;
    int outOfRange = 0;
    double ratios[kCount] = {};
    bool hadSamples[kCount] = {};

    for (size_t i = 0; i < kCount; ++i) {
        const uint64_t num = windowNum_[i].exchange(0, std::memory_order_relaxed);
        const uint64_t den = windowDen_[i].exchange(0, std::memory_order_relaxed);
        hadSamples[i] = den > 0;
        ratios[i] = den > 0 ? static_cast<double>(num) / static_cast<double>(den)
                            : 0.0;
        const bool out = hadSamples[i] && (ratios[i] < kExpectations[i].lo ||
                                           ratios[i] > kExpectations[i].hi);
        if (out) ++outOfRange;
        const int written = std::snprintf(
            line + offset, sizeof(line) - static_cast<size_t>(offset),
            "%s%s=%s%.2f[%.2f-%.2f]%s", offset > 0 ? " " : "", kNames[i],
            hadSamples[i] ? "" : "n/a:",
            ratios[i], kExpectations[i].lo, kExpectations[i].hi,
            out ? "!OUT" : "");
        if (written <= 0 ||
            static_cast<size_t>(offset + written) >= sizeof(line)) {
            break;
        }
        offset += written;
    }

    // 总是打:全部在区间内时,这一行正是唯一能证明策略还在按预期运行的东西。
    // 只有越界才升 Warning(同契约 coverage 的分级)。
    platformLog(outOfRange > 0 ? LogLevel::Warning : LogLevel::Info, "Policy",
                "f=%llu out=%d %s",
                static_cast<unsigned long long>(frameId), outOfRange, line);

    // 越界逐条点名,带上区间依据与归属 —— 这行是要拿去派活的,而且看的人得能判断
    // 是系统坏了还是这个区间当初就定错了。
    for (size_t i = 0; i < kCount; ++i) {
        if (!hadSamples[i]) continue;
        if (ratios[i] >= kExpectations[i].lo &&
            ratios[i] <= kExpectations[i].hi) {
            continue;
        }
        platformLog(LogLevel::Warning, "Policy",
                    "  OUT-OF-RANGE %s=%.3f 期望[%.2f-%.2f] owner=%s | 依据:%s",
                    kNames[i], ratios[i], kExpectations[i].lo,
                    kExpectations[i].hi, kExpectations[i].owner,
                    kExpectations[i].rationale);
    }
}

}  // namespace policy
}  // namespace earth_engine
