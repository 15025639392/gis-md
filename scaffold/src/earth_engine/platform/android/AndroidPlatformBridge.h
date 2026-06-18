#pragma once

#include "../bridge/PlatformBridge.h"

namespace earth_engine {

/// 在 JNI_OnLoad 中调用，初始化 JavaVM 全局引用
void AndroidPlatformBridge_InitJvm(void* jvm);

/// Android 平台桥接。
/// 网络请求走 native libcurl multi scheduler；JNI 仅用于 Android 平台能力。
class AndroidPlatformBridge : public PlatformBridge {
public:
    /// @param jvm JavaVM 指针（从 JNI_OnLoad 获取）
    explicit AndroidPlatformBridge(void* jvm);
    ~AndroidPlatformBridge() override;

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

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace earth_engine
