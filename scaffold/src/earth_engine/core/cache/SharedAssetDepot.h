#pragma once

#include <algorithm>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace earth_engine {

/**
 * @brief Generic LRU-cached asset depot with epoch-based invalidation.
 *
 * Mirrors cesium-native CesiumAsync::SharedAssetDepot pattern:
 * - getOrCreate: cache hit → immediate; in-flight → share; else → create
 * - LRU eviction by byte budget
 * - Epoch-based bulk invalidation
 *
 * @tparam TAsset     Asset type stored in cache.
 * @tparam TKey       Key type for lookup (requires std::hash or KeyTraits).
 * @tparam KeyTraits  Must provide: static std::string toString(const TKey&)
 */
template <typename TAsset, typename TKey, typename KeyTraits>
class SharedAssetDepot {
public:
    using AssetPtr = std::shared_ptr<const TAsset>;
    using Waiter = std::function<void(AssetPtr)>;

    struct InFlightEntry {
        std::vector<Waiter> waiters;
    };

    SharedAssetDepot() = default;

    // --- Cache access ---

    /// Look up cached asset by key. Returns nullptr if not cached.
    AssetPtr get(const TKey& key) {
        std::string sk = KeyTraits::toString(key);
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = cache_.find(sk);
        if (it == cache_.end()) return nullptr;
        touch(sk);
        return it->second;
    }

    /// Store asset in cache under key.
    void put(const TKey& key, AssetPtr asset) {
        std::string sk = KeyTraits::toString(key);
        std::lock_guard<std::mutex> lock(mutex_);
        bool existed = cache_.find(sk) != cache_.end();
        cache_[sk] = asset;
        if (existed) {
            touch(sk);
        } else {
            lru_.push_front(sk);
            lruMap_[sk] = lru_.begin();
        }
        cacheBytes_ += assetSize(asset);
        pruneToBudget();
    }

    /// Remove a single entry from cache.
    void remove(const TKey& key) {
        std::string sk = KeyTraits::toString(key);
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = cache_.find(sk);
        if (it != cache_.end()) {
            cacheBytes_ -= assetSize(it->second);
            auto lruIt = lruMap_.find(sk);
            if (lruIt != lruMap_.end()) {
                lru_.erase(lruIt->second);
                lruMap_.erase(lruIt);
            }
            cache_.erase(it);
        }
    }

    /// Bulk invalidate: clear all cache entries.
    void invalidateAll() {
        std::lock_guard<std::mutex> lock(mutex_);
        cache_.clear();
        lru_.clear();
        lruMap_.clear();
        cacheBytes_ = 0;
        inFlight_.clear();
    }

    /// Check if key is cached.
    bool contains(const TKey& key) {
        std::string sk = KeyTraits::toString(key);
        std::lock_guard<std::mutex> lock(mutex_);
        return cache_.find(sk) != cache_.end();
    }

    // --- In-flight tracking ---

    /// Begin tracking an in-flight request for key.
    /// Returns true if this is the first caller (should start the real request).
    bool startInFlight(const TKey& key, Waiter waiter) {
        std::string sk = KeyTraits::toString(key);
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = inFlight_.find(sk);
        if (it != inFlight_.end()) {
            it->second.waiters.push_back(std::move(waiter));
            return false; // already in flight — share
        }
        InFlightEntry entry;
        entry.waiters.push_back(std::move(waiter));
        inFlight_[sk] = std::move(entry);
        return true; // first caller — should issue request
    }

    /// Complete an in-flight request. Notifies all waiters and caches result.
    void finishInFlight(const TKey& key, AssetPtr result) {
        std::string sk = KeyTraits::toString(key);
        std::vector<Waiter> waiters;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = inFlight_.find(sk);
            if (it != inFlight_.end()) {
                waiters = std::move(it->second.waiters);
                inFlight_.erase(it);
            }
        }
        if (result) {
            put(key, result);
        }
        for (auto& w : waiters) {
            if (w) w(result);
        }
    }

    /// Abort an in-flight request. Notifies waiters with nullptr.
    void abortInFlight(const TKey& key) {
        finishInFlight(key, nullptr);
    }

    /// Check if key is currently in-flight.
    bool isInFlight(const TKey& key) {
        std::string sk = KeyTraits::toString(key);
        std::lock_guard<std::mutex> lock(mutex_);
        return inFlight_.find(sk) != inFlight_.end();
    }

    // --- Budget ---

    /// Current cache byte usage.
    int64_t cacheBytes() const { return cacheBytes_; }

    /// Maximum cache bytes before LRU eviction.
    void setMaxCacheBytes(int64_t bytes) { maxCacheBytes_ = bytes; }
    int64_t maxCacheBytes() const { return maxCacheBytes_; }

private:
    void touch(const std::string& sk) {
        auto lruIt = lruMap_.find(sk);
        if (lruIt != lruMap_.end()) {
            lru_.erase(lruIt->second);
            lru_.push_front(sk);
            lruMap_[sk] = lru_.begin();
        }
    }

    void pruneToBudget() {
        while (cacheBytes_ > maxCacheBytes_ && !lru_.empty()) {
            std::string victim = lru_.back();
            lru_.pop_back();
            lruMap_.erase(victim);
            auto it = cache_.find(victim);
            if (it != cache_.end()) {
                cacheBytes_ -= assetSize(it->second);
                cache_.erase(it);
            }
        }
    }

    static int64_t assetSize(AssetPtr ptr) {
        return ptr ? static_cast<int64_t>(sizeof(TAsset)) : 0;
    }

    std::mutex mutex_;
    std::unordered_map<std::string, AssetPtr> cache_;
    std::deque<std::string> lru_;
    std::unordered_map<std::string, std::deque<std::string>::iterator> lruMap_;
    std::unordered_map<std::string, InFlightEntry> inFlight_;
    int64_t cacheBytes_ = 0;
    int64_t maxCacheBytes_ = 16 * 1024 * 1024; // 16 MiB default
};

} // namespace earth_engine
