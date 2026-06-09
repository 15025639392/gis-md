#pragma once

#include "PersistentCache.h"
#include "../async/AsyncSystem.h"

#include <cstdint>
#include <list>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace earth_engine {

/// Thread-safe LRU cache for raw HTTP response bytes.
/// Layered over PersistentCache for disk persistence.
class HttpCache {
public:
    explicit HttpCache(size_t maxEntries = 2000) : maxEntries_(maxEntries) {}

    std::vector<uint8_t> get(const std::string& url) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = map_.find(url);
            if (it != map_.end()) {
                lru_.splice(lru_.begin(), lru_, it->second.lruIt);
                return it->second.data;
            }
        }
        auto data = PersistentCache::load(url);
        if (!data.empty()) {
            put(url, data);
            return data;
        }
        return {};
    }

    void put(const std::string& url, const std::vector<uint8_t>& data) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = map_.find(url);
            if (it != map_.end()) {
                it->second.data = data;
                lru_.splice(lru_.begin(), lru_, it->second.lruIt);
                return;
            }
            if (map_.size() >= maxEntries_) evictOne();
            lru_.push_front(url);
            map_[url] = {data, lru_.begin()};
        }
        persistAsync(url, data);
    }

    void put(const std::string& url, std::vector<uint8_t>&& data) {
        persistAsync(url, data);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = map_.find(url);
            if (it != map_.end()) {
                it->second.data = std::move(data);
                lru_.splice(lru_.begin(), lru_, it->second.lruIt);
                return;
            }
            if (map_.size() >= maxEntries_) evictOne();
            lru_.push_front(url);
            map_[url] = {std::move(data), lru_.begin()};
        }
    }

    static HttpCache& shared() {
        static HttpCache instance(2000);
        return instance;
    }

private:
    void persistAsync(const std::string& url, const std::vector<uint8_t>& data) {
        if (!PersistentCache::cacheDir().empty() && !data.empty()) {
            std::string u = url;
            std::vector<uint8_t> d = data;
            AsyncSystem::pool().enqueue([u = std::move(u), d = std::move(d)] {
                PersistentCache::save(u, d);
            });
        }
    }

    void evictOne() {
        if (lru_.empty()) return;
        map_.erase(lru_.back());
        lru_.pop_back();
    }

    struct Entry {
        std::vector<uint8_t> data;
        std::list<std::string>::iterator lruIt;
    };

    size_t maxEntries_;
    std::mutex mutex_;
    std::list<std::string> lru_;
    std::unordered_map<std::string, Entry> map_;
};

} // namespace earth_engine
