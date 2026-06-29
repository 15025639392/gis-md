#pragma once

#include "PersistentCache.h"
#include "../async/AsyncSystem.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <list>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace earth_engine {

/// A cached HTTP response with status, headers, body, and expiry.
struct CachedResponse {
    int statusCode = 0;
    std::vector<std::pair<std::string, std::string>> headers;
    std::vector<uint8_t> body;
    std::time_t expiryTime = 0; // 0 = no expiry

    bool isExpired() const {
        return expiryTime > 0 && std::time(nullptr) >= expiryTime;
    }
};

/// Thread-safe LRU cache for HTTP responses with expiry and pruning.
/// Enhanced to match cesium-native CachingAssetAccessor capabilities.
class HttpCache {
public:
    explicit HttpCache(size_t maxEntries = 2000)
        : maxEntries_(maxEntries) {}

    // --- Backward-compat body-only API (returns raw bytes) ---

    /// @deprecated Use get() for full response or getBody() explicitly.
    std::vector<uint8_t> get(const std::string& url) {
        auto resp = getResponse(url);
        return resp ? resp->body : std::vector<uint8_t>{};
    }

    /// @deprecated Use put(url, CachedResponse) for full response.
    void put(const std::string& url, const std::vector<uint8_t>& data) {
        CachedResponse resp;
        resp.body = data;
        putResponse(url, resp);
    }

    void put(const std::string& url, std::vector<uint8_t>&& data) {
        CachedResponse resp;
        resp.body = std::move(data);
        putResponse(url, resp);
    }

    // --- Full response API ---

    /// Get cached response. Returns nullptr if not cached or expired.
    std::shared_ptr<const CachedResponse> getResponse(const std::string& url) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = map_.find(url);
        if (it == map_.end()) return nullptr;
        if (it->second.response.isExpired()) {
            map_.erase(it);
            lruMap_.erase(url);
            return nullptr;
        }
        touch(url);
        return std::make_shared<const CachedResponse>(it->second.response);
    }

    /// Store response in cache.
    void putResponse(const std::string& url, const CachedResponse& response) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = map_.find(url);
            if (it != map_.end()) {
                it->second.response = response;
                touch(url);
                return;
            }
            if (map_.size() >= maxEntries_) evictOne();
            lru_.push_front(url);
            lruMap_[url] = lru_.begin();
            map_[url] = Entry{response, lru_.begin()};
        }
        persistAsync(url, response.body);
    }

    /// Remove a single entry.
    void remove(const std::string& url) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = map_.find(url);
        if (it != map_.end()) {
            lruMap_.erase(url);
            map_.erase(it);
        }
    }

    /// Remove expired entries.
    size_t prune() {
        std::lock_guard<std::mutex> lock(mutex_);
        size_t count = 0;
        auto it = map_.begin();
        while (it != map_.end()) {
            if (it->second.response.isExpired()) {
                lruMap_.erase(it->first);
                it = map_.erase(it);
                ++count;
            } else {
                ++it;
            }
        }
        return count;
    }

    /// Clear all cached entries.
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        map_.clear();
        lru_.clear();
        lruMap_.clear();
    }

    /// Check if URL is cached and not expired.
    bool contains(const std::string& url) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = map_.find(url);
        if (it == map_.end()) return false;
        if (it->second.response.isExpired()) {
            map_.erase(it);
            lruMap_.erase(url);
            return false;
        }
        return true;
    }

    /// Number of entries.
    size_t size() const { return map_.size(); }

    /// Singleton for global use.
    static HttpCache& shared() {
        static HttpCache instance(2000);
        return instance;
    }

    void setMaxEntries(size_t n) { maxEntries_ = n; }
    size_t maxEntries() const { return maxEntries_; }

private:
    void touch(const std::string& url) {
        auto lruIt = lruMap_.find(url);
        if (lruIt != lruMap_.end()) {
            lru_.erase(lruIt->second);
            lru_.push_front(url);
            lruMap_[url] = lru_.begin();
        }
    }

    void evictOne() {
        if (lru_.empty()) return;
        std::string victim = lru_.back();
        lru_.pop_back();
        lruMap_.erase(victim);
        map_.erase(victim);
    }

    void persistAsync(const std::string& url, const std::vector<uint8_t>& body) {
        if (!PersistentCache::cacheDir().empty() && !body.empty()) {
            std::string u = url;
            std::vector<uint8_t> d = body;
            AsyncSystem::pool().enqueue([u = std::move(u), d = std::move(d)] {
                PersistentCache::save(u, d);
            });
        }
    }

    struct Entry {
        CachedResponse response;
        std::list<std::string>::iterator lruIt;
    };

    size_t maxEntries_;
    std::mutex mutex_;
    std::list<std::string> lru_;
    std::unordered_map<std::string, std::list<std::string>::iterator> lruMap_;
    std::unordered_map<std::string, Entry> map_;
};

} // namespace earth_engine
