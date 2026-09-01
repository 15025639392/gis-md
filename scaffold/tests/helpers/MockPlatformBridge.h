#pragma once

#include "earth_engine/platform/bridge/PlatformBridge.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
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
    class PendingHttpRequest final : public HttpRequest {
    public:
        PendingHttpRequest(
            std::atomic<int>* cancelCount,
            std::function<void(int, std::vector<uint8_t>)> callback)
            : cancelCount_(cancelCount), callback_(std::move(callback)) {}
        ~PendingHttpRequest() override { cancel(); }
        void cancel() override {
            if (cancelled_) return;
            cancelled_ = true;
            if (cancelCount_) cancelCount_->fetch_add(1);
            callback_ = nullptr;
        }
    private:
        std::atomic<int>* cancelCount_ = nullptr;
        std::function<void(int, std::vector<uint8_t>)> callback_;
        bool cancelled_ = false;
    };

    /// Seed a URL → response body mapping.
    void setResponse(const std::string& url, std::vector<uint8_t> body) {
        std::lock_guard<std::mutex> lock(mutex_);
        responses_[url] = std::move(body);
    }
    void setPostResponseContaining(const std::string& bodyToken,
                                   std::vector<uint8_t> body) {
        std::lock_guard<std::mutex> lock(mutex_);
        postResponses_.push_back({bodyToken, std::move(body)});
    }
    void setPostResponder(
        std::function<std::vector<uint8_t>(const std::string&)> responder) {
        std::lock_guard<std::mutex> lock(mutex_);
        postResponder_ = std::move(responder);
    }

    // ---- 系统信号 ----
    void onEnterBackground() override {}
    void onEnterForeground() override {}

    // ---- 网络 ----
    std::unique_ptr<HttpRequest> get(
        const std::string& url,
        std::function<void(int, std::vector<uint8_t>)> callback,
        HttpRequestOptions options = {}) override {
        std::vector<uint8_t> response;
        bool found = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++requestCount_;
            requestedUrls_.push_back(url);
            requests_.push_back({"GET", url, {}, options.headers});
            if (hangRequests_) {
                return std::make_unique<PendingHttpRequest>(
                    &cancelCount_, std::move(callback));
            }
            auto it = responses_.find(url);
            if (it != responses_.end()) {
                response = it->second;
                found = true;
            }
        }
        if (callback) callback(found ? 200 : 404, std::move(response));
        return nullptr;
    }

    std::unique_ptr<HttpRequest> post(
        const std::string& url,
        std::vector<uint8_t> body,
        const std::string& contentType,
        std::function<void(int, std::vector<uint8_t>)> callback,
        HttpRequestOptions options = {}) override {
        std::vector<uint8_t> response;
        bool found = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++requestCount_;
            requestedUrls_.push_back(url);
            requests_.push_back({"POST", url, std::move(body),
                                 options.headers});
            requests_.back().headers.emplace_back("Content-Type", contentType);
            if (hangRequests_) {
                return std::make_unique<PendingHttpRequest>(
                    &cancelCount_, std::move(callback));
            }
            const std::string requestBody(requests_.back().body.begin(),
                                          requests_.back().body.end());
            if (postResponder_) {
                response = postResponder_(requestBody);
                found = !response.empty();
            }
            for (const auto& candidate : postResponses_) {
                if (found) break;
                if (requestBody.find(candidate.first) != std::string::npos) {
                    response = candidate.second;
                    found = true;
                    break;
                }
            }
            if (!found) {
                auto it = responses_.find(url);
                if (it != responses_.end()) {
                    response = it->second;
                    found = true;
                }
            }
        }
        if (callback) callback(found ? 200 : 404, std::move(response));
        return nullptr;
    }

    // ---- 文件系统 ----
    std::string cacheDirectory() const override { return "/tmp/mock_cache"; }
    std::string documentsDirectory() const override { return "/tmp/mock_docs"; }

    // ---- 图片解码 ----
    std::unique_ptr<DecodedImage> decodeImage(
        const uint8_t*, size_t) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!decodedImage_) return nullptr;
        return std::make_unique<DecodedImage>(*decodedImage_);
    }
    void setDecodedImage(DecodedImage image) {
        std::lock_guard<std::mutex> lock(mutex_);
        decodedImage_ = std::move(image);
    }

    // ---- 日志 ----
    void log(LogLevel, const std::string&, const std::string&) override {}

    // ---- 设备 ----
    DeviceInfo deviceInfo() const override { return DeviceInfo{}; }

    // ---- 鉴权 ----
    std::string getToken(const std::string&) const override { return ""; }

    int requestCount() const { return requestCount_; }
    int cancelCount() const { return cancelCount_.load(); }
    void setHangRequests(bool hang) {
        std::lock_guard<std::mutex> lock(mutex_);
        hangRequests_ = hang;
    }

    std::vector<std::string> requestedUrls() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return requestedUrls_;
    }

    struct RequestRecord {
        std::string method;
        std::string url;
        std::vector<uint8_t> body;
        std::vector<std::pair<std::string, std::string>> headers;
    };
    std::vector<RequestRecord> requests() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return requests_;
    }

private:
    mutable std::mutex mutex_;
    std::map<std::string, std::vector<uint8_t>> responses_;
    std::optional<DecodedImage> decodedImage_;
    std::vector<std::pair<std::string, std::vector<uint8_t>>> postResponses_;
    std::function<std::vector<uint8_t>(const std::string&)> postResponder_;
    int requestCount_ = 0;
    std::vector<std::string> requestedUrls_;
    std::vector<RequestRecord> requests_;
    bool hangRequests_ = false;
    std::atomic<int> cancelCount_{0};
};

} // namespace testing
} // namespace earth_engine
