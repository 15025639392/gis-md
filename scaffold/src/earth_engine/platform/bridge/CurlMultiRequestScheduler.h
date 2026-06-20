#pragma once

#include "PlatformBridge.h"

#include <memory>
#include <string>
#include <vector>
#include <chrono>
#include <functional>

namespace earth_engine {

class CurlMultiRequestScheduler {
public:
    static constexpr int kDefaultMaximumActiveRequests = 20;

    static CurlMultiRequestScheduler& shared();

    explicit CurlMultiRequestScheduler(
        int maximumActiveRequests = kDefaultMaximumActiveRequests);
    ~CurlMultiRequestScheduler();

    CurlMultiRequestScheduler(const CurlMultiRequestScheduler&) = delete;
    CurlMultiRequestScheduler& operator=(const CurlMultiRequestScheduler&) = delete;

    std::unique_ptr<HttpRequest> get(
        const std::string& url,
        std::function<void(int statusCode, std::vector<uint8_t> body)> callback,
        HttpRequestOptions options);
    std::unique_ptr<HttpRequest> post(
        const std::string& url,
        std::vector<uint8_t> body,
        const std::string& contentType,
        std::function<void(int statusCode, std::vector<uint8_t> body)> callback,
        HttpRequestOptions options);
    std::vector<uint8_t> getBlocking(
        const std::string& url,
        HttpRequestOptions options = {},
        std::chrono::milliseconds timeout = std::chrono::seconds(20),
        std::function<bool()> shouldCancel = {});

    void cancelQueuedRequests();
    void shutdown();
    int maximumActiveRequests() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace earth_engine
