#pragma once

#include <memory>
#include <string>
#include <cstdint>
#include <functional>

namespace earth_engine {

/// 解码后的图片（像素缓冲区）。
/// 由 PlatformBridge 的图片解码器产生，在线程池中解码后回传主线程上传 GPU。
struct DecodedImage {
    int width = 0;
    int height = 0;
    int channels = 0;           // 3 (RGB) or 4 (RGBA)
    std::vector<uint8_t> pixels; // RGBA or RGB (row-major)
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

enum class LogLevel { Debug, Info, Warning, Error };

enum class NetworkStatus { Online, Metered, Offline };

enum class HttpRequestPriority {
    Low = 0,
    Normal = 1,
    High = 2
};

struct HttpRequestOptions {
    HttpRequestPriority priority = HttpRequestPriority::Normal;
};

/// 平台桥接抽象接口。
/// 引擎核心通过此接口获取平台能力（网络、文件、图片解码、日志、设备信息），
/// 不直接依赖 iOS SDK 或 Android SDK。
class PlatformBridge {
public:
    virtual ~PlatformBridge() = default;

    // ---- 系统信号 ----
    virtual void onMemoryPressure() = 0;
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

} // namespace earth_engine
