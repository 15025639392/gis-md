#include "earth_engine/threading/RenderThreadPlacement.h"

#include "earth_engine/debug/PlatformLog.h"

#include <android/api-level.h>
#include <dlfcn.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <vector>

// 渲染线程放置策略的 Android 实现。三级降级的来龙去脉见 RenderThreadPlacement.h;
// 这里只记录各级的实现细节与真机实测结论。
namespace earth_engine {
namespace {

constexpr const char* kLogTag = "RenderThreadPlacement";

using UpdateTargetFn = int (*)(void*, int64_t);
using ReportActualFn = int (*)(void*, int64_t);
using CloseSessionFn = void (*)(void*);

// sched_setattr 的内核入参结构 —— bionic 既没有包装函数也没有这个类型的头文件,
// 所以自己声明并直接走系统调用。
struct SchedAttr {
    uint32_t size;
    uint32_t schedPolicy;
    uint64_t schedFlags;
    int32_t schedNice;
    uint32_t schedPriority;
    uint64_t schedRuntime;
    uint64_t schedDeadline;
    uint64_t schedPeriod;
    uint32_t schedUtilMin;
    uint32_t schedUtilMax;
};

bool setCurrentThreadUclampMin(unsigned utilMin) {
    constexpr uint64_t kKeepPolicy = 0x08;
    constexpr uint64_t kKeepParams = 0x10;
    constexpr uint64_t kUtilClampMin = 0x20;
    SchedAttr attr{};
    attr.size = sizeof(SchedAttr);
    attr.schedFlags = kKeepPolicy | kKeepParams | kUtilClampMin;
    attr.schedUtilMin = utilMin;
    return syscall(__NR_sched_setattr, 0, &attr, 0u) == 0;
}

long readCpuMaxFreqKHz(int cpu) {
    char path[128];
    std::snprintf(path, sizeof(path),
                  "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq",
                  cpu);
    FILE* file = std::fopen(path, "r");
    if (!file) {
        return 0;
    }
    long value = 0;
    if (std::fscanf(file, "%ld", &value) != 1) {
        value = 0;
    }
    std::fclose(file);
    return value;
}

// 性能核 = cpuinfo_max_freq 高于最低档的所有核心(即"除最小簇之外")。按运行时
// 拓扑判定,不写死核号 —— 大小核数量与编号在不同 SoC 上并不一致。
bool pinCurrentThreadToPerformanceCores(int* pinnedCountOut) {
    const int cpuCount = static_cast<int>(sysconf(_SC_NPROCESSORS_CONF));
    if (cpuCount <= 1) {
        return false;
    }
    long minMaxFreq = 0;
    std::vector<long> freqs(static_cast<size_t>(cpuCount), 0);
    for (int cpu = 0; cpu < cpuCount; ++cpu) {
        freqs[static_cast<size_t>(cpu)] = readCpuMaxFreqKHz(cpu);
        const long freq = freqs[static_cast<size_t>(cpu)];
        if (freq > 0 && (minMaxFreq == 0 || freq < minMaxFreq)) {
            minMaxFreq = freq;
        }
    }
    if (minMaxFreq == 0) {
        return false;  // 读不到拓扑就不要乱绑。
    }
    cpu_set_t set;
    CPU_ZERO(&set);
    int pinned = 0;
    for (int cpu = 0; cpu < cpuCount; ++cpu) {
        if (freqs[static_cast<size_t>(cpu)] > minMaxFreq) {
            CPU_SET(cpu, &set);
            ++pinned;
        }
    }
    // 同构 SoC(全部核心同频)→ 没有"性能簇"可言,绑了反而只是缩小可用集合。
    if (pinned == 0 || pinned == cpuCount) {
        return false;
    }
    if (sched_setaffinity(0, sizeof(set), &set) != 0) {
        return false;
    }
    if (pinnedCountOut) {
        *pinnedCountOut = pinned;
    }
    return true;
}

}  // namespace

// ADPF(Android Dynamic Performance Framework)hint session 与降级状态。
//
// 符号 API 33 引入而引擎 minSdk 是 26,所以全部走 dlsym 运行时解析 —— 低版本上
// 拿不到函数指针,自动退化为「不发提示」并下探 ②③。
struct RenderThreadPlacement::Impl {
    Status status;
    void* session = nullptr;
    int64_t targetNanos = 0;
    UpdateTargetFn updateTarget = nullptr;
    ReportActualFn reportActual = nullptr;
    CloseSessionFn closeSession = nullptr;

    // 必须在目标线程内调用(登记的是 gettid())。
    bool attachPerformanceHint(int64_t targetWorkDurationNanos) {
        using GetManagerFn = void* (*)();
        using CreateSessionFn = void* (*)(void*, const int32_t*, size_t, int64_t);

        auto getManager = reinterpret_cast<GetManagerFn>(
            dlsym(RTLD_DEFAULT, "APerformanceHint_getManager"));
        auto createSession = reinterpret_cast<CreateSessionFn>(
            dlsym(RTLD_DEFAULT, "APerformanceHint_createSession"));
        updateTarget = reinterpret_cast<UpdateTargetFn>(
            dlsym(RTLD_DEFAULT, "APerformanceHint_updateTargetWorkDuration"));
        reportActual = reinterpret_cast<ReportActualFn>(
            dlsym(RTLD_DEFAULT, "APerformanceHint_reportActualWorkDuration"));
        closeSession = reinterpret_cast<CloseSessionFn>(
            dlsym(RTLD_DEFAULT, "APerformanceHint_closeSession"));

        if (!getManager || !createSession || !reportActual) {
            platformLog(LogLevel::Info, kLogTag,
                        "PerfHint unavailable (API<33) — 不发提示,下探 uclamp/affinity");
            return false;
        }
        void* manager = getManager();
        if (!manager) {
            platformLog(LogLevel::Info, kLogTag, "PerfHint getManager returned null");
            return false;
        }
        // 设备是否真的实现了 hint session:preferredRate <= 0 = PowerHAL 没接。
        using PreferredRateFn = int64_t (*)(void*);
        auto preferredRate = reinterpret_cast<PreferredRateFn>(
            dlsym(RTLD_DEFAULT, "APerformanceHint_getPreferredUpdateRateNanos"));
        const int64_t rateNanos = preferredRate ? preferredRate(manager) : -1;
        const int32_t tid = static_cast<int32_t>(gettid());
        errno = 0;
        session = createSession(manager, &tid, 1, targetWorkDurationNanos);
        platformLog(LogLevel::Info, kLogTag,
                    "PerfHint probe preferredRate=%lldns createErrno=%d api=%d",
                    static_cast<long long>(rateNanos),
                    errno,
                    android_get_device_api_level());
        targetNanos = targetWorkDurationNanos;
        platformLog(LogLevel::Info, kLogTag,
                    "PerfHint session=%p tid=%d target=%.2fms",
                    session,
                    tid,
                    static_cast<double>(targetWorkDurationNanos) / 1e6);
        return session != nullptr;
    }
};

RenderThreadPlacement::RenderThreadPlacement()
    : impl_(std::make_unique<Impl>()) {}

RenderThreadPlacement::~RenderThreadPlacement() {
    release();
}

RenderThreadPlacement::Status RenderThreadPlacement::applyToCurrentThread(
    const Options& options) {
    // nice 是 per-thread 的。单靠它救不了放置(实测 setpriority(-8) rc=0 但放置
    // 完全不变 —— nice 只影响同一 runqueue 内的时间片权重、不参与选核),设它只为
    // 对齐 Android 自家 RenderThread 的 display 优先级语义。
    setpriority(PRIO_PROCESS, 0, options.threadNice);

    // 同一个实例被复用到新线程时(surface 重建 → 渲染线程重起),旧 session 绑的是
    // 已死的 tid,先关掉再重新登记。
    release();
    impl_->status = Status{};
    const auto targetNanos =
        static_cast<int64_t>(options.targetFrameMs * 1e6);
    // ① 生效时就不再下探 ②③ —— 让系统自己调度总比我们写死好。
    if (impl_->attachPerformanceHint(targetNanos)) {
        impl_->status.mode = Mode::PerformanceHint;
    } else if (setCurrentThreadUclampMin(options.utilClampMin)) {
        impl_->status.mode = Mode::UtilClampMin;
    } else {
        int pinnedCores = 0;
        if (pinCurrentThreadToPerformanceCores(&pinnedCores)) {
            impl_->status.mode = Mode::PerformanceCoreAffinity;
            impl_->status.pinnedCoreCount = pinnedCores;
        }
    }
    impl_->status.threadNice = getpriority(PRIO_PROCESS, 0);
    return impl_->status;
}

void RenderThreadPlacement::reportActualWorkDurationMs(double actualMs) {
    if (!impl_->session || !impl_->reportActual) {
        return;
    }
    const int64_t nanos = static_cast<int64_t>(actualMs * 1e6);
    // 0 或负数会被系统当非法参数拒绝。
    impl_->reportActual(impl_->session, nanos > 0 ? nanos : 1);
}

void RenderThreadPlacement::setTargetFrameMs(double targetFrameMs) {
    const auto targetNanos = static_cast<int64_t>(targetFrameMs * 1e6);
    if (!impl_->session || !impl_->updateTarget ||
        targetNanos == impl_->targetNanos) {
        return;
    }
    impl_->updateTarget(impl_->session, targetNanos);
    impl_->targetNanos = targetNanos;
}

void RenderThreadPlacement::release() {
    if (impl_->session && impl_->closeSession) {
        impl_->closeSession(impl_->session);
    }
    impl_->session = nullptr;
}

RenderThreadPlacement::Status RenderThreadPlacement::status() const {
    return impl_->status;
}

}  // namespace earth_engine
