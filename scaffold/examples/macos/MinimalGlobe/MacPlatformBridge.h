#pragma once

#include "earth_engine/platform/bridge/PlatformBridge.h"

namespace earth_engine {

/// macOS 平台桥接实现
/// 使用 NSURLSession 进行 HTTP 请求，NSImage 解码图片
class MacPlatformBridge : public PlatformBridge {
public:
    MacPlatformBridge() = default;
    ~MacPlatformBridge() override = default;

    void onMemoryPressure() override {}
    void onEnterBackground() override {}
    void onEnterForeground() override {}

    std::unique_ptr<HttpRequest> get(
        const std::string& url,
        std::function<void(int statusCode, std::vector<uint8_t> body)> callback) override;

    std::string cacheDirectory() const override;
    std::string documentsDirectory() const override;

    std::unique_ptr<DecodedImage> decodeImage(const uint8_t* data, size_t len) override;

    void log(LogLevel level, const std::string& tag, const std::string& message) override;

    DeviceInfo deviceInfo() const override;

    std::string getToken(const std::string& providerId) const override { return ""; }
};

} // namespace earth_engine
