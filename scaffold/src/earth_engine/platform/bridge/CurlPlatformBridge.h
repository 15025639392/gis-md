#pragma once

#include "PlatformBridge.h"

namespace earth_engine {

/// 基于 libcurl + stb_image 的跨平台 PlatformBridge 实现。
/// 用于桌面端开发和 Android/iOS MVP 阶段。
class CurlPlatformBridge : public PlatformBridge {
public:
    CurlPlatformBridge();
    ~CurlPlatformBridge() override;

    // ---- 系统信号 ----
    void onMemoryPressure() override;
    void onEnterBackground() override;
    void onEnterForeground() override;

    // ---- 网络 ----
    std::unique_ptr<HttpRequest> get(
        const std::string& url,
        std::function<void(int statusCode, std::vector<uint8_t> body)> callback,
        HttpRequestOptions options = {}) override;

    // ---- 文件系统 ----
    std::string cacheDirectory() const override;
    std::string documentsDirectory() const override;

    // ---- 图片解码 ----
    std::unique_ptr<DecodedImage> decodeImage(
        const uint8_t* data, size_t len) override;

    // ---- 日志 ----
    void log(LogLevel level, const std::string& tag,
             const std::string& message) override;

    // ---- 设备 ----
    DeviceInfo deviceInfo() const override;

    // ---- 鉴权 ----
    std::string getToken(const std::string& providerId) const override;
};

} // namespace earth_engine
