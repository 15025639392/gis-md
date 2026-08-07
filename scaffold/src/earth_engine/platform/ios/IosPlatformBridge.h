#pragma once

#include "../bridge/PlatformBridge.h"

struct IosPlatformBridgeImpl;

namespace earth_engine {

/// iOS PlatformBridge — NSURLSession 网络 / Keychain 鉴权 / ImageIO 解码 /
/// UIKit 设备信息。结构对齐 macOS 的 MacPlatformBridge，仅设备信息使用 UIKit。
///
/// 生产实现，取代示例内联的 IosDemoPlatformBridge（后者用已废弃的
/// NSURLConnection 同步请求且 get() 签名过时）。
class IosPlatformBridge final : public PlatformBridge {
public:
    IosPlatformBridge();
    ~IosPlatformBridge() override;

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
    IosPlatformBridgeImpl* impl_;
};

} // namespace earth_engine
