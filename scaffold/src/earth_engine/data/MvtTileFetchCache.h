#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "MvtDecoder.h"
#include "../tiling/TileKey.h"
#include "../threading/CancellationToken.h"
#include "../core/async/AsyncSystem.h"
#include "../debug/PerfTimer.h"
#include "../tiling/TileRetryBackoffPolicy.h"

namespace earth_engine {

/// 矢量数据瓦 fetch+decode 的共享缓存(LRU + 在途合并)。
///
/// 从刀1 `VectorDrapeImageryProvider::State` 提炼:刀2 起同一批 z14 祖先瓦
/// 有**两个消费者**(面 drape 栅格化 + 线 SDF 场烘焙),不共享就是同瓦双份
/// fetch+decode。overzoom 下多个页并发要同一张祖先瓦是常态,在途合并必需。
///
/// **E3 泛化(E 方案通路):载荷类型模板化。** MVT 与高德 Nebula 共用同一
/// 套获取层(LRU/在途合并/失败重试/L1 解码 + L2 字节),差异只在「字节流
/// 解成什么」——decode 经模板参数注入:decodeMvtTile → MvtTile,amap 的
/// decode → Feature 列表。现有 MVT 消费方用别名
/// `MvtTileFetchCache = MvtTileFetchCacheT<MvtTile, MvtTileDecodeTraits>`
/// 零改动接入。
///
/// 契约:
/// - `request` 可在任意线程调;命中时**同步回调**(调用栈内),miss 时在
///   fetch 完成线程回调 —— 调用方不得假设回调线程。
/// - 失败(fetch 非 200/decode 失败)回调 nullptr;**有界重试**(2026-08-20
///   用户契约):同一瓦片失败后最多重试 2 次(间隔按 TileRetryBackoffPolicy
///   指数退避),用尽即记终止失败、不再发网络 —— 此前"失败不入缓存"让死源
///   在页存储 churn 下每帧重打服务器(实测同 key 53 次/5s 风暴)。失败账本
///   有界(容量满挤最旧),旧 key 被挤掉后仍可自愈。
/// - 拆除竞态(tombstone 法则):fetch 回调只捕 shared_ptr<State>,本对象
///   先亡不悬垂;回调必到(取消语义由调用方在自己的回调里处理)。
/// **两层容量(2026-08-15,P2 结清)**:解码瓦 ~450KB/张、压缩字节 ~33KB/张
/// (实测最密 z14:1104 要素中 MvtFeature 头就占 164KB,属性 map 78KB),
/// 13.6× 差距 —— 靠加解码层容量兜住工作集要付 ~57MB,不划算。改为:
///   L1 解码层(小):服务在途合并与热复用,容量按并发消费者规模定;
///   L2 字节层(大):L1 淘汰时降级到这里,命中则**重解码**(实测 5.72ms/瓦
///                   桌面,手机 2-3×,在 decodePool 上跑不占渲染线程)。
/// 换来的是网络重拉归零,代价 ~33KB/张而不是 ~450KB/张。
template <typename Payload, typename DecodeTraits>
class MvtTileFetchCacheT {
public:
    using FetchCallback = std::function<void(int, std::vector<uint8_t>)>;
    using FetchFn = std::function<void(const TileKey&, FetchCallback)>;
    using TileCallback = std::function<void(std::shared_ptr<const Payload>)>;

    /// @param capacity        L1 解码瓦上限
    /// @param rawCapacity     L2 压缩字节上限(0 = 不启用 L2)
    /// @param decodePool      L2 命中时的重解码线程池。**为空则就地解码**
    ///                        —— host 测试用;真机必须传,否则 5-17ms 落在
    ///                        调用线程(渲染线程)上。
    MvtTileFetchCacheT(FetchFn fetch, size_t capacity, size_t rawCapacity = 0,
                       std::shared_ptr<ThreadPool> decodePool = nullptr);

    /// 取一张数据瓦(z/x/y 语义,schemeId 透传给 FetchFn)。
    void request(const TileKey& key, TileCallback callback);

    struct Stats {
        uint64_t hits = 0;
        uint64_t fetches = 0;
        /// 重复拉取:同一 key 被 fetch 过一次以上(= 曾被淘汰又要回来)。
        /// 容量是否够用的**直接判据**,比命中率好读:稳态该恒 0。
        uint64_t refetches = 0;
        /// L2 命中(免了一次网络往返,付一次重解码)
        uint64_t rawHits = 0;
        size_t residentTiles = 0;
        size_t rawTiles = 0;
        size_t rawBytes = 0;
        /// 常驻解码瓦的近似字节数(几何点 + 属性表 + 容器头,见
        /// approxTileBytes)。容量决策与 V18「内存有界」都要这个数 ——
        /// 不量就只能填"应该很小",那是空头承诺。
        size_t residentBytes = 0;
        /// 退避/终止跳过的请求数(诊断:死源风暴应随时间趋稳为 0 增长)。
        uint64_t failureSkips = 0;
    };
    Stats stats() const;

private:
    struct State {
        std::mutex mutex;
        struct CacheEntry {
            uint64_t key;
            std::shared_ptr<const Payload> tile;
            size_t bytes = 0;
            /// 原始压缩字节:L1 淘汰时降级进 L2,免掉一次网络往返。
            std::shared_ptr<const std::vector<uint8_t>> body;
        };
        std::list<CacheEntry> order;  // 头新尾旧
        std::unordered_map<uint64_t,
                          typename std::list<CacheEntry>::iterator>
            map;
        /// 失败账本(2026-08-20 有界重试):失败瓦片不占 L1/L2,单独记
        /// retryNotBeforeMs + 失败次数;请求命中此账本时按
        /// TileRetryBackoffPolicy 决定跳过(退避中/用尽)或放行重试。
        struct FailureEntry {
            double retryNotBeforeMs = 0.0;
            int failureCount = 0;
            uint64_t seq = 0;
        };
        std::unordered_map<uint64_t, FailureEntry> failures;
        uint64_t failureSeq = 0;
        /// 失败账本容量上限(满则挤最旧,旧 key 到期后允许重新探测=自愈)。
        size_t maxFailures = 2048;
        uint64_t failureSkips = 0;
        /// L2:压缩字节层。L1 淘汰时降级到此,命中则重解码(见类注释)。
        struct RawEntry {
            uint64_t key;
            std::shared_ptr<const std::vector<uint8_t>> body;
        };
        std::list<RawEntry> rawOrder;
        std::unordered_map<uint64_t,
                          typename std::list<RawEntry>::iterator>
            rawMap;
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
        void insertDecoded(
            uint64_t dk, std::shared_ptr<const Payload> tile,
            std::shared_ptr<const std::vector<uint8_t>> body) {
            order.push_front(
                CacheEntry{dk, std::move(tile), 0, std::move(body)});
            order.begin()->bytes = DecodeTraits::approxBytes(*order.begin()->tile);
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
        void demoteToRaw(
            uint64_t dk,
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

    FetchFn fetch_;
    /// L2 命中的重解码去处;为空则就地解码(见构造注释)。
    std::shared_ptr<ThreadPool> decodePool_;
    std::shared_ptr<State> state_;
};

namespace detail {

inline uint64_t packDataKey(int z, int x, int y) {
    return (static_cast<uint64_t>(z) << 48) |
           (static_cast<uint64_t>(static_cast<uint32_t>(x)) << 24) |
           static_cast<uint64_t>(static_cast<uint32_t>(y));
}

} // namespace detail

template <typename Payload, typename DecodeTraits>
MvtTileFetchCacheT<Payload, DecodeTraits>::MvtTileFetchCacheT(
    FetchFn fetch, size_t capacity, size_t rawCapacity,
    std::shared_ptr<ThreadPool> decodePool)
    : fetch_(std::move(fetch)),
      decodePool_(std::move(decodePool)),
      state_(std::make_shared<State>()) {
    state_->capacity = std::max<size_t>(1, capacity);
    state_->rawCapacity = rawCapacity;
}

template <typename Payload, typename DecodeTraits>
void MvtTileFetchCacheT<Payload, DecodeTraits>::request(
    const TileKey& key, TileCallback callback) {
    if (!callback) return;
    if (!fetch_) {
        callback(nullptr);
        return;
    }
    const uint64_t dk = detail::packDataKey(key.z, key.x, key.y);
    bool needFetch = false;
    bool failImmediately = false;
    std::shared_ptr<const Payload> hit;
    std::shared_ptr<const std::vector<uint8_t>> rawHit;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        // 失败账本闸:用尽(不再重试)或退避中(未到期)→ 不发网络,直接失败
        // 回调(调用方照画透明/回退);到期才放行一次重试。
        auto fit = state_->failures.find(dk);
        if (fit != state_->failures.end() &&
            (fit->second.failureCount >
                 TileRetryBackoffPolicy::kMaxSourceRetries ||
             !TileRetryBackoffPolicy::isRetryDue(
                 fit->second.retryNotBeforeMs, perf::nowMs()))) {
            ++state_->failureSkips;
            failImmediately = true;
        }
        if (!failImmediately) {
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
                                                state_->rawOrder,
                                                rawIt->second);
                        rawHit = rawIt->second->body;
                        ++state_->rawHits;
                        // 与网络路径同样走 inflight:重解码期间的并发请求搭车,
                        // 不重复解码(解码 5-17ms,并发重复会成倍烧 worker)。
                        state_->inflight[dk].push_back(std::move(callback));
                    } else {
                        state_->inflight[dk].push_back(std::move(callback));
                        needFetch = true;
                        ++state_->fetches;
                        if (++state_->fetchCount[dk] > 1) {
                            ++state_->refetches;
                        }
                    }
                }
            }
        }
    }
    if (failImmediately) {
        callback(nullptr);
        return;
    }
    if (hit) {
        callback(std::move(hit));
        return;
    }
    if (rawHit) {
        std::shared_ptr<State> state = state_;
        auto work = [state, dk, rawHit]() {
            std::shared_ptr<const Payload> tile;
            auto decoded = std::make_shared<Payload>();
            if (DecodeTraits::decode(rawHit->data(), rawHit->size(), *decoded,
                                     nullptr)) {
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
        std::shared_ptr<const Payload> tile;
        std::shared_ptr<const std::vector<uint8_t>> shared;
        if (statusCode == 200 && !body.empty()) {
            auto decoded = std::make_shared<Payload>();
            if (DecodeTraits::decode(body.data(), body.size(), *decoded,
                                     nullptr)) {
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
                // 成功:复位失败账本,后续直接命中缓存。
                state->failures.erase(dk);
            } else {
                // 失败:记入账本 —— 第 1 次失败后 500ms 重试,第 2 次 1s,
                // 超过 kMaxSourceRetries 用尽不再重试(有界,防死源风暴)。
                auto& f = state->failures[dk];
                f.failureCount =
                    std::min(f.failureCount + 1,
                             TileRetryBackoffPolicy::kMaxSourceRetries + 1);
                if (f.failureCount >
                    TileRetryBackoffPolicy::kMaxSourceRetries) {
                    f.retryNotBeforeMs = std::numeric_limits<double>::max();
                } else {
                    f.retryNotBeforeMs =
                        perf::nowMs() +
                        TileRetryBackoffPolicy::backoffMs(f.failureCount);
                }
                f.seq = ++state->failureSeq;
                if (state->failures.size() > state->maxFailures) {
                    auto oldest = state->failures.begin();
                    for (auto it = state->failures.begin();
                         it != state->failures.end(); ++it) {
                        if (it->second.seq < oldest->second.seq) {
                            oldest = it;
                        }
                    }
                    state->failures.erase(oldest);
                }
            }
        }
        for (TileCallback& cb : waiters) {
            cb(tile);
        }
    });
}

template <typename Payload, typename DecodeTraits>
typename MvtTileFetchCacheT<Payload, DecodeTraits>::Stats
MvtTileFetchCacheT<Payload, DecodeTraits>::stats() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return Stats{state_->hits,          state_->fetches,
                 state_->refetches,     state_->rawHits,
                 state_->map.size(),    state_->rawMap.size(),
                 state_->rawBytes,      state_->residentBytes,
                 state_->failureSkips};
}

/// MVT 解码特质:MvtTile 载荷 + decodeMvtTile 字节解码 + 近似字节统计。
struct MvtTileDecodeTraits {
    static bool decode(const uint8_t* data, size_t size, MvtTile& out,
                       std::string* error) {
        return decodeMvtTile(data, size, out, error);
    }

    /// 解码瓦的近似常驻字节。只数**堆上真正持有的**:几何点、属性字符串、
    /// 各级 vector/map 的节点开销。SSO(≤15 字节)不额外计堆 —— MVT 的 key
    /// 多是 name/kind/rank 这类短串,算进去会高估一倍。
    static size_t approxBytes(const MvtTile& tile) {
        // unordered_map 每节点 ≈ 指针 + hash + pair,取 56B(libc++ 实测量级)
        constexpr size_t kMapNodeBytes = 56;
        constexpr size_t kVecHeaderBytes = 24;
        size_t bytes =
            sizeof(MvtTile) + tile.layers.capacity() * sizeof(MvtLayer);
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
};

/// MVT 消费方别名:载荷 = 解码后的 MVT 瓦片。语义与旧的
/// `MvtTileFetchCache` 完全一致,调用方零改动。
using MvtTileFetchCache = MvtTileFetchCacheT<MvtTile, MvtTileDecodeTraits>;

} // namespace earth_engine
