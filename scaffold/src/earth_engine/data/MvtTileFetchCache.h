#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "MvtDecoder.h"
#include "../tiling/TileKey.h"
#include "../threading/CancellationToken.h"

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
/// - 失败(fetch 非 200/decode 失败)回调 nullptr,且**不入缓存**:下一批
///   请求会重试,server 恢复后自愈。
/// - 拆除竞态(tombstone 法则):fetch 回调只捕 shared_ptr<State>,本对象
///   先亡不悬垂;回调必到(取消语义由调用方在自己的回调里处理)。
class MvtTileFetchCache {
public:
    using FetchCallback = std::function<void(int, std::vector<uint8_t>)>;
    using FetchFn = std::function<void(const TileKey&, FetchCallback)>;
    using TileCallback =
        std::function<void(std::shared_ptr<const MvtTile>)>;

    MvtTileFetchCache(FetchFn fetch, size_t capacity);

    /// 取一张数据瓦(z/x/y 语义,schemeId 透传给 FetchFn)。
    void request(const TileKey& key, TileCallback callback);

    struct Stats {
        uint64_t hits = 0;
        uint64_t fetches = 0;
    };
    Stats stats() const;

private:
    struct State;
    FetchFn fetch_;
    std::shared_ptr<State> state_;
};

} // namespace earth_engine
