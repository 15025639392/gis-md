#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <cstdint>
#include <functional>
#include <vector>
#include <utility>

#include "../../debug/PlatformLog.h"  // earth_engine::LogLevel（引擎统一日志级别）

namespace earth_engine {

/// 解码后的图片（像素缓冲区）。
/// 由 PlatformBridge 的图片解码器产生，在线程池中解码后回传主线程上传 GPU。
struct DecodedImage {
    int width = 0;
    int height = 0;
    int channels = 0;           // 1 (R), 3 (RGB), or 4 (RGBA)
    int bytesPerChannel = 1;
    std::vector<uint8_t> pixels; // row-major channel bytes
};

/// 设备信息
struct DeviceInfo {
    std::string platform;       // "iOS" | "Android"
    std::string model;          // e.g. "iPhone15,2"
    std::string osVersion;
    float screenDensity = 1.0f; // 1.0 / 2.0 / 3.0
    int screenWidthPx = 0;
    int screenHeightPx = 0;
    int cpuCores = 0;
    int64_t totalMemoryBytes = 0;
};

// LogLevel 已上移到 debug/PlatformLog.h（引擎统一日志级别），经上面的 include 复用，
// 成员保持 { Debug, Info, Warning, Error } 不变。

enum class NetworkStatus { Online, Metered, Offline };

enum class HttpRequestPriority {
    Low = 0,
    Normal = 1,
    High = 2
};

struct HttpRequestOptions {
    using Header = std::pair<std::string, std::string>;
    HttpRequestOptions() = default;
    HttpRequestOptions(HttpRequestPriority requestPriority,
                       std::vector<Header> requestHeaders = {})
        : priority(requestPriority), headers(std::move(requestHeaders)) {}

    HttpRequestPriority priority = HttpRequestPriority::Normal;
    std::vector<Header> headers;
    /// 可选响应头输出（I-P1）：非空时，请求完成回调触发前填充响应头列表，
    /// 供上层 HttpCache 解析 Cache-Control/Expires/ETag 计算过期与重验。
    /// 为空 = 不收集（行为与旧版完全一致，零额外开销）。
    /// 由网络线程填充；回调返回后调用方可在任意线程读取。
    std::shared_ptr<std::vector<Header>> responseHeaders;
    /// 外部取消旗标(通常来自 CancellationToken::sharedFlag)。置位后调度器
    /// 工作线程在下一轮 wake(≤50ms)中止传输/丢弃排队项;回调仍恰好一次
    /// (以 code=-1 触发)——上层的 retired-token 清账依赖这一点。
    /// 为空 = 该请求不可被外部取消(行为与旧版完全一致)。
    std::shared_ptr<std::atomic<bool>> cancelFlag;
    /// 动态优先级 cell(值 = HttpRequestPriority 的 int)。上层随帧更新;
    /// 调度器工作线程每轮 wake 把仍在排队、cell 与所在桶不一致的请求搬到
    /// 目标桶(promotion 插到 High 即插队,demotion 落到低桶尾)。**只影响
    /// 未开始的排队项** —— 在飞传输不可抢占(curl 无此语义,cesium-native
    /// 同样只重排待发队列)。为空 = 静态优先级(行为与旧版一致)。
    std::shared_ptr<std::atomic<int>> priorityCell;
};

/// 平台桥接抽象接口。
/// 引擎核心通过此接口获取平台能力（网络、文件、图片解码、日志、设备信息），
/// 不直接依赖 iOS SDK 或 Android SDK。
class PlatformBridge {
public:
    virtual ~PlatformBridge() = default;

    // ---- 系统信号 ----
    virtual void onEnterBackground() = 0;
    virtual void onEnterForeground() = 0;

    // ---- 网络 ----
    /// 发起 HTTP GET 请求。
    /// @param url 请求 URL
    /// @param callback 回调（在后台线程调用）。参数：(statusCode, body bytes)
    /// @return 取消句柄（析构时取消请求）
    virtual std::unique_ptr<class HttpRequest> get(
        const std::string& url,
        std::function<void(int statusCode, std::vector<uint8_t> body)> callback,
        HttpRequestOptions options = {}) = 0;
    virtual std::unique_ptr<class HttpRequest> post(
        const std::string& url,
        std::vector<uint8_t> body,
        const std::string& contentType,
        std::function<void(int statusCode, std::vector<uint8_t> body)> callback,
        HttpRequestOptions options = {});
    virtual int maximumActiveRequests() const { return -1; }

    // ---- 文件系统 ----
    virtual std::string cacheDirectory() const = 0;
    virtual std::string documentsDirectory() const = 0;

    // ---- 图片解码 ----
    /// 在线程池中调用。解码图片为 RGBA 像素缓冲区。
    virtual std::unique_ptr<DecodedImage> decodeImage(
        const uint8_t* data, size_t len) = 0;

    // ---- 日志 ----
    virtual void log(LogLevel level, const std::string& tag,
                     const std::string& message) = 0;

    // ---- 设备 ----
    virtual DeviceInfo deviceInfo() const = 0;

    // ---- 鉴权 ----
    /// 获取 provider 的 API token（从 Keychain/Keystore 读取）
    virtual std::string getToken(const std::string& providerId) const = 0;
};

/// HTTP 请求句柄（RAII 取消）。
class HttpRequest {
public:
    virtual ~HttpRequest() = default;
    virtual void cancel() = 0;
};

inline std::unique_ptr<HttpRequest> PlatformBridge::post(
    const std::string&,
    std::vector<uint8_t>,
    const std::string&,
    std::function<void(int statusCode, std::vector<uint8_t> body)> callback,
    HttpRequestOptions) {
    if (callback) {
        callback(-1, {});
    }
    return nullptr;
}

} // namespace earth_engine
