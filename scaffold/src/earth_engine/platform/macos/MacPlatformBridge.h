#pragma once

#include "../bridge/PlatformBridge.h"

struct MacPlatformBridgeImpl;

namespace earth_engine {

/// macOS PlatformBridge — NSURLSession 网络 / Keychain 鉴权 / CoreGraphics 解码
class MacPlatformBridge final : public PlatformBridge {
public:
    MacPlatformBridge();
    ~MacPlatformBridge() override;

    // ---- 系统 ----
    void onEnterBackground() override;
    void onEnterForeground() override;

    // ---- 网络 ----
    std::unique_ptr<HttpRequest> get(
        const std::string& url,
        std::function<void(int statusCode, std::vector<uint8_t> body)> callback,
        HttpRequestOptions options = {}) override;

    // ---- 文件 ----
    std::string cacheDirectory() const override;
    std::string documentsDirectory() const override;

    // ---- 图像解码 ----
    std::unique_ptr<DecodedImage> decodeImage(
        const uint8_t* data, size_t len) override;

    // ---- 日志 ----
    void log(LogLevel level, const std::string& tag,
             const std::string& message) override;

    // ---- 设备 ----
    DeviceInfo deviceInfo() const override;

    // ---- 鉴权 ----
    std::string getToken(const std::string& providerId) const override;

private:
    MacPlatformBridgeImpl* impl_;
};

} // namespace earth_engine
