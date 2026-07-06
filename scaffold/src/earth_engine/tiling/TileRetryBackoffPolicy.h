#pragma once

namespace earth_engine {

/// 瞬时加载失败(TileLoadStatus::RetryLater → FailedTemporarily)的指数退避。
///
/// 背景/取舍:cesium-native 对 RetryLater 不做退避,交给客户端;本项目按
/// AGENTS.md「体验优先/异常处理」判断,让 FailedTemporarily 瓦片每帧(~16ms)
/// 重打服务器是明显的坏行为(对限流/抖动源=自造 DoS),故加**项目自有的**退避
/// 层(相对 cesium-native 的有意偏离)。纯函数,便于单测。
struct TileRetryBackoffPolicy {
    /// 首次失败的退避,之后每次失败翻倍。
    static constexpr double kBaseMs = 500.0;
    /// 退避上限:持续失败的源最多每 30s 重试一次。
    static constexpr double kCapMs = 30000.0;

    /// 第 attemptCount 次失败后的退避毫秒数(attemptCount 从 1 起)。
    /// 500 → 1000 → 2000 → … 封顶 30000。
    static double backoffMs(int attemptCount) {
        if (attemptCount <= 1) {
            return kBaseMs;
        }
        double ms = kBaseMs;
        for (int i = 1; i < attemptCount && ms < kCapMs; ++i) {
            ms *= 2.0;
        }
        return ms < kCapMs ? ms : kCapMs;
    }

    /// 到达/越过 retryNotBeforeMs 才允许重试。retryNotBeforeMs==0 视为可立即重试
    /// (从未失败或已重置)。
    static bool isRetryDue(double retryNotBeforeMs, double nowMs) {
        return nowMs >= retryNotBeforeMs;
    }
};

} // namespace earth_engine
