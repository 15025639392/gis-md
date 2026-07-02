#pragma once

#include <cstdarg>
#include <cstdio>

#ifdef __ANDROID__
#include <android/log.h>
#endif

namespace earth_engine {

/// 引擎统一的日志级别（全局唯一定义）。PlatformBridge::log 等也复用此枚举，
/// 因此成员保持 { Debug, Info, Warning, Error } 与既有调用点一致。
enum class LogLevel { Debug, Info, Warning, Error };

/// 统一跨平台日志：Android -> logcat，其它平台 -> stderr。
///
/// 这是整个引擎中唯一允许出现日志相关 `#ifdef __ANDROID__` 的地方。中间层
/// (scene / camera / providers / terrain / tiling 等) 一律调用 platformLog(...)，
/// 不再各自写平台分支——从而做到中间层零平台宏。
///
/// printf 风格；`format(printf, 3, 4)` 让编译器校验格式串与实参匹配。
#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 3, 4)))
#endif
inline void platformLog(LogLevel level, const char* tag, const char* fmt, ...) {
    char buffer[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

#ifdef __ANDROID__
    int priority = ANDROID_LOG_INFO;
    switch (level) {
        case LogLevel::Debug:   priority = ANDROID_LOG_DEBUG; break;
        case LogLevel::Info:    priority = ANDROID_LOG_INFO;  break;
        case LogLevel::Warning: priority = ANDROID_LOG_WARN;  break;
        case LogLevel::Error:   priority = ANDROID_LOG_ERROR; break;
    }
    __android_log_print(priority, tag, "%s", buffer);
#else
    (void)level;
    std::fprintf(stderr, "[%s] %s\n", tag, buffer);
#endif
}

} // namespace earth_engine
