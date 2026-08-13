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

} // namespace

/// 账本 shared_ptr 持有:fetch 回调只捕它,cache 对象先亡不悬垂。
struct MvtTileFetchCache::State {
    std::mutex mutex;
    struct CacheEntry {
        uint64_t key;
        std::shared_ptr<const MvtTile> tile;
    };
    std::list<CacheEntry> order;  // 头新尾旧
    std::unordered_map<uint64_t, std::list<CacheEntry>::iterator> map;
    std::unordered_map<uint64_t, std::vector<TileCallback>> inflight;
    size_t capacity = 48;
    uint64_t hits = 0;
    uint64_t fetches = 0;
};

MvtTileFetchCache::MvtTileFetchCache(FetchFn fetch, size_t capacity)
    : fetch_(std::move(fetch)), state_(std::make_shared<State>()) {
    state_->capacity = std::max<size_t>(1, capacity);
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
                state_->inflight[dk].push_back(std::move(callback));
                needFetch = true;
                ++state_->fetches;
            }
        }
    }
    if (hit) {
        callback(std::move(hit));
        return;
    }
    if (!needFetch) return;

    std::shared_ptr<State> state = state_;
    fetch_(key, [state, dk](int statusCode, std::vector<uint8_t> body) {
        std::shared_ptr<const MvtTile> tile;
        if (statusCode == 200 && !body.empty()) {
            auto decoded = std::make_shared<MvtTile>();
            if (decodeMvtTile(body.data(), body.size(), *decoded)) {
                tile = std::move(decoded);
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
                state->order.push_front(State::CacheEntry{dk, tile});
                state->map[dk] = state->order.begin();
                while (state->map.size() > state->capacity) {
                    state->map.erase(state->order.back().key);
                    state->order.pop_back();
                }
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
    return Stats{state_->hits, state_->fetches};
}

} // namespace earth_engine
