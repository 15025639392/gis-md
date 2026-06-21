#include "QuantizedMeshLayerJsonFetcher.h"
#include "../core/cache/HttpCache.h"
#include "../platform/bridge/CurlMultiRequestScheduler.h"
#include "../platform/bridge/PlatformBridge.h"

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>

namespace earth_engine {
namespace {

bool isCesiumSuccessfulHttpStatus(int statusCode) {
    return statusCode == 0 || (statusCode >= 200 && statusCode < 300);
}

std::string cacheKeyFor(
    const std::string& url,
    const std::vector<HttpRequestOptions::Header>& headers) {
    if (headers.empty()) {
        return url;
    }
    std::string key = url;
    key += "\nheaders:";
    for (const auto& header : headers) {
        key += "\n";
        key += header.first;
        key += ":";
        key += header.second;
    }
    return key;
}

} // namespace

QuantizedMeshLayerJsonFetcher::QuantizedMeshLayerJsonFetcher(
    PlatformBridge* platformBridge)
    : platformBridge_(platformBridge) {}

std::vector<uint8_t> QuantizedMeshLayerJsonFetcher::fetchBlocking(
    const std::string& url,
    HttpRequestPriority priority,
    const std::vector<HttpRequestOptions::Header>& headers,
    CancelPredicate shouldCancel) const {
    const std::string cacheKey = cacheKeyFor(url, headers);
    auto cached = HttpCache::shared().get(cacheKey);
    if (!cached.empty()) return cached;

    constexpr const char* kFilePrefix = "file://";
    if (url.rfind(kFilePrefix, 0) == 0) {
        std::ifstream in(url.substr(std::strlen(kFilePrefix)), std::ios::binary);
        if (!in) return {};
        std::vector<uint8_t> data{
            std::istreambuf_iterator<char>(in),
            std::istreambuf_iterator<char>()};
        HttpCache::shared().put(cacheKey, data);
        return data;
    }

    if (platformBridge_) {
        struct WaitState {
            std::vector<uint8_t> result;
            std::mutex mutex;
            std::condition_variable cv;
            bool done = false;
        };
        auto state = std::make_shared<WaitState>();
        auto request = platformBridge_->get(
            url,
            [state](int code, std::vector<uint8_t> body) {
                {
                    std::lock_guard<std::mutex> lk(state->mutex);
                    if (isCesiumSuccessfulHttpStatus(code)) {
                        state->result = std::move(body);
                    }
                    state->done = true;
                }
                state->cv.notify_one();
            },
            {priority, headers});

        bool done = false;
        {
            const auto deadline = std::chrono::steady_clock::now() +
                std::chrono::seconds(20);
            std::unique_lock<std::mutex> lk(state->mutex);
            while (!state->done && !(shouldCancel && shouldCancel())) {
                const auto now = std::chrono::steady_clock::now();
                if (now >= deadline) break;
                state->cv.wait_until(
                    lk,
                    std::min(deadline, now + std::chrono::milliseconds(20)));
            }
            done = state->done;
        }
        if ((!done || (shouldCancel && shouldCancel())) && request) {
            request->cancel();
        }
        if (done && !state->result.empty()) {
            HttpCache::shared().put(cacheKey, state->result);
        }
        return done ? std::move(state->result) : std::vector<uint8_t>{};
    }

    std::vector<uint8_t> result =
        CurlMultiRequestScheduler::shared().getBlocking(
            url,
            {priority, headers},
            std::chrono::seconds(20),
            std::move(shouldCancel));
    if (!result.empty()) HttpCache::shared().put(cacheKey, result);
    return result;
}

} // namespace earth_engine
