#include "MvtTileFetchCache.h"

#include <algorithm>
#include <list>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace earth_engine {

namespace {

uint64_t packDataKey(int z, int x, int y) {
    return (static_cast<uint64_t>(z) << 48) |
           (static_cast<uint64_t>(static_cast<uint32_t>(x)) << 24) |
           static_cast<uint64_t>(static_cast<uint32_t>(y));
}

/// 解码瓦的近似常驻字节。只数**堆上真正持有的**:几何点、属性字符串、
/// 各级 vector/map 的节点开销。SSO(≤15 字节)不额外计堆 —— MVT 的 key
/// 多是 name/kind/rank 这类短串,算进去会高估一倍。
size_t approxTileBytes(const MvtTile& tile) {
    // unordered_map 每节点 ≈ 指针 + hash + pair,取 56B(libc++ 实测量级)
    constexpr size_t kMapNodeBytes = 56;
    constexpr size_t kVecHeaderBytes = 24;
    size_t bytes = sizeof(MvtTile) + tile.layers.capacity() * sizeof(MvtLayer);
    for (const MvtLayer& layer : tile.layers) {
        bytes += layer.name.size() > 15 ? layer.name.size() : 0;
        bytes += layer.features.capacity() * sizeof(MvtFeature);
        for (const MvtFeature& f : layer.features) {
            bytes += f.paths.capacity() * kVecHeaderBytes;
            for (const auto& path : f.paths) {
                bytes += path.capacity() * sizeof(MvtPoint);
            }
            bytes += f.properties.size() * kMapNodeBytes;
            for (const auto& kv : f.properties) {
                if (kv.first.size() > 15) bytes += kv.first.size();
                if (kv.second.size() > 15) bytes += kv.second.size();
            }
        }
    }
    return bytes;
}

} // namespace

/// 账本 shared_ptr 持有:fetch 回调只捕它,cache 对象先亡不悬垂。
struct MvtTileFetchCache::State {
    std::mutex mutex;
    struct CacheEntry {
        uint64_t key;
        std::shared_ptr<const MvtTile> tile;
        size_t bytes = 0;
        /// 原始压缩字节:L1 淘汰时降级进 L2,免掉一次网络往返。
        std::shared_ptr<const std::vector<uint8_t>> body;
    };
    std::list<CacheEntry> order;  // 头新尾旧
    std::unordered_map<uint64_t, std::list<CacheEntry>::iterator> map;
    /// L2:压缩字节层。L1 淘汰时降级到此,命中则重解码(见类注释)。
    struct RawEntry {
        uint64_t key;
        std::shared_ptr<const std::vector<uint8_t>> body;
    };
    std::list<RawEntry> rawOrder;
    std::unordered_map<uint64_t, std::list<RawEntry>::iterator> rawMap;
    std::unordered_map<uint64_t, std::vector<TileCallback>> inflight;
    /// 曾经 fetch 过的 key(只增,用于判重复拉取)。只存 8 字节 key,
    /// 千级瓦量下自身开销可忽略;它衡量的正是"容量不够导致的白拉"。
    std::unordered_map<uint64_t, uint32_t> fetchCount;
    size_t capacity = 48;
    size_t rawCapacity = 0;
    uint64_t hits = 0;
    uint64_t rawHits = 0;
    uint64_t fetches = 0;
    uint64_t refetches = 0;
    size_t residentBytes = 0;
    size_t rawBytes = 0;

    /// L1 插入 + 溢出降级到 L2(调用方持锁)。
    void insertDecoded(uint64_t dk, std::shared_ptr<const MvtTile> tile,
                       std::shared_ptr<const std::vector<uint8_t>> body) {
        order.push_front(CacheEntry{dk, std::move(tile), 0, std::move(body)});
        order.begin()->bytes = approxTileBytes(*order.begin()->tile);
        map[dk] = order.begin();
        residentBytes += order.begin()->bytes;
        while (map.size() > capacity) {
            CacheEntry& victim = order.back();
            residentBytes -= victim.bytes;
            demoteToRaw(victim.key, std::move(victim.body));
            map.erase(victim.key);
            order.pop_back();
        }
    }

    /// 降级:只留压缩字节(~33KB vs ~450KB)。调用方持锁。
    void demoteToRaw(uint64_t dk,
                     std::shared_ptr<const std::vector<uint8_t>> body) {
        if (rawCapacity == 0 || !body || body->empty()) return;
        auto it = rawMap.find(dk);
        if (it != rawMap.end()) {
            rawOrder.splice(rawOrder.begin(), rawOrder, it->second);
            return;
        }
        rawBytes += body->size();
        rawOrder.push_front(RawEntry{dk, std::move(body)});
        rawMap[dk] = rawOrder.begin();
        while (rawMap.size() > rawCapacity) {
            rawBytes -= rawOrder.back().body->size();
            rawMap.erase(rawOrder.back().key);
            rawOrder.pop_back();
        }
    }
};

MvtTileFetchCache::MvtTileFetchCache(FetchFn fetch, size_t capacity,
                                     size_t rawCapacity,
                                     std::shared_ptr<ThreadPool> decodePool)
    : fetch_(std::move(fetch)),
      decodePool_(std::move(decodePool)),
      state_(std::make_shared<State>()) {
    state_->capacity = std::max<size_t>(1, capacity);
    state_->rawCapacity = rawCapacity;
}

void MvtTileFetchCache::request(const TileKey& key, TileCallback callback) {
    if (!callback) return;
    if (!fetch_) {
        callback(nullptr);
        return;
    }
    const uint64_t dk = packDataKey(key.z, key.x, key.y);
    bool needFetch = false;
    std::shared_ptr<const MvtTile> hit;
    std::shared_ptr<const std::vector<uint8_t>> rawHit;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        auto it = state_->map.find(dk);
        if (it != state_->map.end()) {
            state_->order.splice(state_->order.begin(), state_->order,
                                 it->second);
            hit = it->second->tile;
            ++state_->hits;
        } else {
            auto inIt = state_->inflight.find(dk);
            if (inIt != state_->inflight.end()) {
                inIt->second.push_back(std::move(callback));
            } else {
                // L2:字节还在 → 免网络,重解码即可(见类注释的两层取舍)。
                auto rawIt = state_->rawMap.find(dk);
                if (rawIt != state_->rawMap.end()) {
                    state_->rawOrder.splice(state_->rawOrder.begin(),
                                            state_->rawOrder, rawIt->second);
                    rawHit = rawIt->second->body;
                    ++state_->rawHits;
                    // 与网络路径同样走 inflight:重解码期间的并发请求搭车,
                    // 不重复解码(解码 5-17ms,并发重复会成倍烧 worker)。
                    state_->inflight[dk].push_back(std::move(callback));
                } else {
                    state_->inflight[dk].push_back(std::move(callback));
                    needFetch = true;
                    ++state_->fetches;
                    if (++state_->fetchCount[dk] > 1) ++state_->refetches;
                }
            }
        }
    }
    if (hit) {
        callback(std::move(hit));
        return;
    }
    if (rawHit) {
        std::shared_ptr<State> state = state_;
        auto work = [state, dk, rawHit]() {
            std::shared_ptr<const MvtTile> tile;
            auto decoded = std::make_shared<MvtTile>();
            if (decodeMvtTile(rawHit->data(), rawHit->size(), *decoded)) {
                tile = std::move(decoded);
            }
            std::vector<TileCallback> waiters;
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                auto it = state->inflight.find(dk);
                if (it != state->inflight.end()) {
                    waiters = std::move(it->second);
                    state->inflight.erase(it);
                }
                if (tile) state->insertDecoded(dk, tile, rawHit);
            }
            for (TileCallback& cb : waiters) cb(tile);
        };
        // ⚠️ 无池则就地解码:5-17ms 会落在调用线程(真机上是渲染线程)。
        // 只允许 host 测试走这条;真机构造必须传池。
        if (decodePool_) decodePool_->enqueue(std::move(work));
        else work();
        return;
    }
    if (!needFetch) return;

    std::shared_ptr<State> state = state_;
    fetch_(key, [state, dk](int statusCode, std::vector<uint8_t> body) {
        std::shared_ptr<const MvtTile> tile;
        std::shared_ptr<const std::vector<uint8_t>> shared;
        if (statusCode == 200 && !body.empty()) {
            auto decoded = std::make_shared<MvtTile>();
            if (decodeMvtTile(body.data(), body.size(), *decoded)) {
                tile = std::move(decoded);
                shared = std::make_shared<const std::vector<uint8_t>>(
                    std::move(body));
            }
        }
        std::vector<TileCallback> waiters;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            auto it = state->inflight.find(dk);
            if (it != state->inflight.end()) {
                waiters = std::move(it->second);
                state->inflight.erase(it);
            }
            if (tile) {
                state->insertDecoded(dk, tile, std::move(shared));
            }
            // 失败不入缓存:下一批请求重试,server 恢复后自愈。
        }
        for (TileCallback& cb : waiters) {
            cb(tile);
        }
    });
}

MvtTileFetchCache::Stats MvtTileFetchCache::stats() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return Stats{state_->hits,          state_->fetches,
                 state_->refetches,     state_->rawHits,
                 state_->map.size(),    state_->rawMap.size(),
                 state_->rawBytes,      state_->residentBytes};
}

} // namespace earth_engine
