#pragma once

#include <chrono>
#include <cstdio>

#include "PlatformLog.h"

namespace earth_engine {
namespace perf {

inline double nowMs() {
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

inline bool shouldLog(uint64_t frameId) {
    if (frameId == 0) return false;
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
