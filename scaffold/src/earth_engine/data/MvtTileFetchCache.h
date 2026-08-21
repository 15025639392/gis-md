#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "MvtDecoder.h"
#include "../tiling/TileKey.h"
#include "../threading/CancellationToken.h"
#include "../core/async/AsyncSystem.h"

namespace earth_engine {

/// MVT 数据瓦 fetch+decode 的共享缓存(LRU + 在途合并)。
///
/// 从刀1 `VectorDrapeImageryProvider::State` 提炼:刀2 起同一批 z14 祖先瓦
/// 有**两个消费者**(面 drape 栅格化 + 线 SDF 场烘焙),不共享就是同瓦双份
/// fetch+decode。overzoom 下多个页并发要同一张祖先瓦是常态,在途合并必需。
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
class MvtTileFetchCache {
public:
    using FetchCallback = std::function<void(int, std::vector<uint8_t>)>;
    using FetchFn = std::function<void(const TileKey&, FetchCallback)>;
    using TileCallback =
        std::function<void(std::shared_ptr<const MvtTile>)>;

    /// @param capacity        L1 解码瓦上限
    /// @param rawCapacity     L2 压缩字节上限(0 = 不启用 L2)
    /// @param decodePool      L2 命中时的重解码线程池。**为空则就地解码**
    ///                        —— host 测试用;真机必须传,否则 5-17ms 落在
    ///                        调用线程(渲染线程)上。
    MvtTileFetchCache(FetchFn fetch, size_t capacity, size_t rawCapacity = 0,
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
    struct State;
    FetchFn fetch_;
    /// L2 命中的重解码去处;为空则就地解码(见构造注释)。
    std::shared_ptr<ThreadPool> decodePool_;
    std::shared_ptr<State> state_;
};

} // namespace earth_engine
