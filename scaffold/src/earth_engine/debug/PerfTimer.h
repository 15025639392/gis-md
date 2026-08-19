#pragma once

#include <chrono>
#include <cstdio>
#include <ctime>

#include "PlatformLog.h"

#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif

namespace earth_engine {
namespace perf {

#if defined(__ANDROID__)
// 运行时逐帧日志开关:adb shell setprop debug.ee.perflog 1 → 每帧都打 EarthPerf
// (默认只打 frame≤3 与 %120 节拍帧)。2026-08-20:25ms 慢帧阈值漏掉了 17-24ms
// 的"越 60Hz 预算帧"(每帧差 2-7ms → 掉到 30fps,惯性滑动卡)。
inline bool perFrameLogEnabled() {
    static int cached = -1;
    char prop[4] = {0};
    if (__system_property_get("debug.ee.perflog", prop) > 0) {
        const int v = prop[0] - '0';
        if (v != cached) {
            cached = v;
        }
        return cached == 1;
    }
    return false;
}
#else
inline bool perFrameLogEnabled() { return false; }
#endif

inline double nowMs() {
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

/// 当前线程的 CPU 时间(毫秒)。Linux/Android 用 CLOCK_THREAD_CPUTIME_ID,
/// 其余平台回落墙钟 —— 墙钟会被同机线程抢占膨胀,CPU 时间才是真成本。
/// 2026-08-20 交互瓶颈专项:判 19ms prepareCpuWork 是纯 CPU 还是被共享池
/// 抢占的墙钟伪影。
inline double cpuThreadMs() {
#if defined(__linux__) || defined(__ANDROID__)
    struct timespec ts;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) == 0) {
        return static_cast<double>(ts.tv_sec) * 1000.0 +
               static_cast<double>(ts.tv_nsec) / 1e6;
    }
#endif
    return nowMs();
}

inline bool shouldLog(uint64_t frameId) {
    if (frameId == 0) return false;
    if (perFrameLogEnabled()) return true;
    return frameId <= 3 || (frameId % 120) == 0;
}

inline void logTiming(uint64_t frameId,
                      const char* scope,
                      double elapsedMs,
                      const char* detail = "") {
    constexpr double kSlowFrameMs = 25.0;
    if (!shouldLog(frameId) && elapsedMs < kSlowFrameMs) return;

    platformLog(LogLevel::Info, "EarthPerf",
        "frame=%llu scope=%s ms=%.3f %s",
        static_cast<unsigned long long>(frameId),
        scope,
        elapsedMs,
        detail ? detail : "");
}

inline void logTimingAtLeast(uint64_t frameId,
                             const char* scope,
                             double elapsedMs,
                             double thresholdMs,
                             const char* detail = "") {
    if (!shouldLog(frameId) && elapsedMs < thresholdMs) return;

    platformLog(LogLevel::Info, "EarthPerf",
        "frame=%llu scope=%s ms=%.3f %s",
        static_cast<unsigned long long>(frameId),
        scope,
        elapsedMs,
        detail ? detail : "");
}

class ScopedTimer {
public:
    ScopedTimer(uint64_t frameId, const char* scope, const char* detail = "")
        : frameId_(frameId),
          scope_(scope),
          detail_(detail),
          startMs_(nowMs()) {}

    ~ScopedTimer() {
        logTiming(frameId_, scope_, nowMs() - startMs_, detail_);
    }

    ScopedTimer(const ScopedTimer&) = delete;
    ScopedTimer& operator=(const ScopedTimer&) = delete;

private:
    uint64_t frameId_;
    const char* scope_;
    const char* detail_;
    double startMs_;
};

} // namespace perf
} // namespace earth_engine
