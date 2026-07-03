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
    /// @param appContext Android Context 的 JNI global ref（建议传
    ///        applicationContext）。构造时经 JNI 一次性查询缓存/文件目录与
    ///        设备信息并缓存，之后不再触碰。可为空：空时目录退回内置默认
    ///        路径、设备信息退回 native 可得的字段。所有权归调用方，桥不
    ///        释放该 global ref。
    explicit AndroidPlatformBridge(void* jvm, void* appContext = nullptr);
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
    std::unique_ptr<HttpRequest> post(
        const std::string& url,
        std::vector<uint8_t> body,
        const std::string& contentType,
        std::function<void(int statusCode, std::vector<uint8_t> body)> callback,
        HttpRequestOptions options = {}) override;
    int maximumActiveRequests() const override;

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
