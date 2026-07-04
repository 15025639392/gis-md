#pragma once

#include "PersistentCache.h"
#include "../async/AsyncSystem.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <list>
#include <memory>
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
///
/// P1-7:条目存 shared_ptr<const CachedResponse>——命中直接返回共享引用
/// (零拷贝、锁内零分配),put 全链 move,持久化任务捕获同一 shared_ptr。
/// 容量双限:条数 + body 字节预算(旧实现 2000 条×75KB ≈ 150MB 无上限)。
class HttpCache {
public:
    static constexpr size_t kDefaultMaxBodyBytes = 128ull * 1024 * 1024;

    explicit HttpCache(size_t maxEntries = 2000,
                       size_t maxBodyBytes = kDefaultMaxBodyBytes)
        : maxEntries_(maxEntries), maxBodyBytes_(maxBodyBytes) {}

    // --- Backward-compat body-only API (returns raw bytes) ---

    /// @deprecated Use getResponse() to share the cached body without a copy.
    std::vector<uint8_t> get(const std::string& url) {
        auto resp = getResponse(url);
        return resp ? resp->body : std::vector<uint8_t>{};
    }

    /// @deprecated Use put(url, std::move(data)) or putResponse().
    void put(const std::string& url, const std::vector<uint8_t>& data) {
        CachedResponse resp;
        resp.body = data;
        putResponse(url,
                    std::make_shared<const CachedResponse>(std::move(resp)));
    }

    void put(const std::string& url, std::vector<uint8_t>&& data) {
        CachedResponse resp;
        resp.body = std::move(data);
        putResponse(url,
                    std::make_shared<const CachedResponse>(std::move(resp)));
    }

    // --- Full response API ---

    /// Get cached response. Returns nullptr if not cached or expired.
    /// The returned pointer shares storage with the cache entry — no copy.
    std::shared_ptr<const CachedResponse> getResponse(const std::string& url) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = map_.find(url);
        if (it == map_.end()) return nullptr;
        if (it->second.response->isExpired()) {
            eraseLocked(it);
            return nullptr;
        }
        touch(url);
        return it->second.response;
    }

    /// Store response in cache (copies once into shared storage).
    void putResponse(const std::string& url, const CachedResponse& response) {
        putResponse(url, std::make_shared<const CachedResponse>(response));
    }

    /// Store an already-shared response — zero copy; the persistence task
    /// captures the same shared_ptr.
    void putResponse(const std::string& url,
                     std::shared_ptr<const CachedResponse> response) {
        if (!response) return;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = map_.find(url);
            if (it != map_.end()) {
                currentBodyBytes_ -= it->second.response->body.size();
                currentBodyBytes_ += response->body.size();
                it->second.response = response;
                touch(url);
            } else {
                lru_.push_front(url);
                lruMap_[url] = lru_.begin();
                currentBodyBytes_ += response->body.size();
                map_[url] = Entry{response};
            }
            while ((map_.size() > maxEntries_ ||
                    currentBodyBytes_ > maxBodyBytes_) &&
                   !lru_.empty()) {
                evictOne();
            }
        }
        persistAsync(url, std::move(response));
    }

    /// Remove a single entry.
    void remove(const std::string& url) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = map_.find(url);
        if (it != map_.end()) {
            eraseLocked(it);
        }
    }

    /// Remove expired entries.
    size_t prune() {
        std::lock_guard<std::mutex> lock(mutex_);
        size_t count = 0;
        auto it = map_.begin();
        while (it != map_.end()) {
            if (it->second.response->isExpired()) {
                currentBodyBytes_ -= it->second.response->body.size();
                auto lruIt = lruMap_.find(it->first);
                if (lruIt != lruMap_.end()) {
                    lru_.erase(lruIt->second);
                    lruMap_.erase(lruIt);
                }
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
        currentBodyBytes_ = 0;
    }

    /// Check if URL is cached and not expired.
    bool contains(const std::string& url) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = map_.find(url);
        if (it == map_.end()) return false;
        if (it->second.response->isExpired()) {
            eraseLocked(it);
            return false;
        }
        return true;
    }

    /// Number of entries.
    size_t size() const { return map_.size(); }

    /// Total cached body bytes (approximate, headers excluded).
    size_t bodyBytes() const { return currentBodyBytes_; }

    /// Singleton for global use.
    static HttpCache& shared() {
        static HttpCache instance(2000);
        return instance;
    }

    void setMaxEntries(size_t n) { maxEntries_ = n; }
    size_t maxEntries() const { return maxEntries_; }

private:
    struct Entry {
        std::shared_ptr<const CachedResponse> response;
    };

    void touch(const std::string& url) {
        auto lruIt = lruMap_.find(url);
        if (lruIt != lruMap_.end()) {
            lru_.erase(lruIt->second);
            lru_.push_front(url);
            lruMap_[url] = lru_.begin();
        }
    }

    // LRU 位置一律经 lruMap_ 查(touch 后它是唯一保持同步的索引)。
    void eraseLocked(std::unordered_map<std::string, Entry>::iterator it) {
        currentBodyBytes_ -= it->second.response->body.size();
        auto lruIt = lruMap_.find(it->first);
        if (lruIt != lruMap_.end()) {
            lru_.erase(lruIt->second);
            lruMap_.erase(lruIt);
        }
        map_.erase(it);
    }

    void evictOne() {
        if (lru_.empty()) return;
        std::string victim = lru_.back();
        lru_.pop_back();
        lruMap_.erase(victim);
        auto it = map_.find(victim);
        if (it != map_.end()) {
            currentBodyBytes_ -= it->second.response->body.size();
            map_.erase(it);
        }
    }

    void persistAsync(const std::string& url,
                      std::shared_ptr<const CachedResponse> response) {
        if (!PersistentCache::cacheDir().empty() && !response->body.empty()) {
            AsyncSystem::pool().enqueue(
                [u = url, response = std::move(response)] {
                    PersistentCache::save(u, response->body);
                });
        }
    }

    size_t maxEntries_;
    size_t maxBodyBytes_;
    size_t currentBodyBytes_ = 0;
    std::mutex mutex_;
    std::list<std::string> lru_;
    std::unordered_map<std::string, std::list<std::string>::iterator> lruMap_;
    std::unordered_map<std::string, Entry> map_;
};

} // namespace earth_engine
