#pragma once

#include "earth_engine/platform/bridge/PlatformBridge.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace earth_engine {
namespace testing {

/**
 * @brief Mock PlatformBridge for headless integration testing.
 *
 * Allows pre-seeding HTTP responses by URL, simulating
 * terrain/imagery tile downloads without real network.
 */
class MockPlatformBridge final : public PlatformBridge {
public:
    /// Seed a URL → response body mapping.
    void setResponse(const std::string& url, std::vector<uint8_t> body) {
        std::lock_guard<std::mutex> lock(mutex_);
        responses_[url] = std::move(body);
    }

    // ---- 系统信号 ----
    void onMemoryPressure() override {}
    void onEnterBackground() override {}
    void onEnterForeground() override {}

    // ---- 网络 ----
    std::unique_ptr<HttpRequest> get(
        const std::string& url,
        std::function<void(int, std::vector<uint8_t>)> callback,
        HttpRequestOptions = {}) override {
        std::lock_guard<std::mutex> lock(mutex_);
        ++requestCount_;
        auto it = responses_.find(url);
        if (it != responses_.end() && callback) {
            callback(200, it->second);
        } else if (callback) {
            callback(404, {});
        }
        return nullptr;
    }

    // ---- 文件系统 ----
    std::string cacheDirectory() const override { return "/tmp/mock_cache"; }
    std::string documentsDirectory() const override { return "/tmp/mock_docs"; }

    // ---- 图片解码 ----
    std::unique_ptr<DecodedImage> decodeImage(
        const uint8_t*, size_t) override {
        return nullptr;
    }

    // ---- 日志 ----
    void log(LogLevel, const std::string&, const std::string&) override {}

    // ---- 设备 ----
    DeviceInfo deviceInfo() const override { return DeviceInfo{}; }

    // ---- 鉴权 ----
    std::string getToken(const std::string&) const override { return ""; }

    int requestCount() const { return requestCount_; }

private:
    mutable std::mutex mutex_;
    std::map<std::string, std::vector<uint8_t>> responses_;
    int requestCount_ = 0;
};

} // namespace testing
} // namespace earth_engine
