#pragma once

#include "TileKey.h"
#include "TileLoadQueue.h"
#include "TileLoadTypes.h"
#include "TilePlan.h"
#include "TileSelectionCounters.h"
#include "TileTraversalDetails.h"
#include "TilesetTile.h"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <unordered_map>
#include <vector>

namespace earth_engine {

// ③ 增量切面(见 docs/issues/selector-incremental-frontier-design-2026-07-06.md)。
//
// 每帧全量遍历时,对每个访问过的子树捕获其「贡献」= 该子树根 visitTileIfNeeded
// 期间净新增到 visibleTiles / loadQueue 的条目 + counter 增量 + 返回的
// traversalDetails + 子树内到阈值的最小 margin。捕获在**子树 visit 退出时**做,
// 故子树内部的 kick trim / restore-load-queue erase 都已结算,[snapshot, 末尾)
// 区间即净贡献(ground truth)。
//
// Layer 1:只捕获、不剪枝——输出恒等于全量(§8 oracle 验证)。Layer 3 才用这些
// 缓存在 clean 子树处跳过重算并拼接贡献。
struct SubtreeSelectionCache {
    // 该子树净新增的 visibleTiles(DFS 顺序切片)。
    std::vector<TileKey> rendered;
    // 该子树净新增的 loadQueue 请求。
    std::vector<TileLoadRequest> loads;
    // 该子树对选择计数的增量贡献。
    TileSelectionCounters counterDelta;
    // 子树根 visitTileIfNeeded 的返回聚合。
    TileTraversalDetails details;
    // 子树内 |SSE − maximumScreenSpaceError| 的最小值(离翻转最近的瓦片)。
    // Layer 3 用它 + 本帧翻转带宽判定子树是否稳定。
    double minMargin = std::numeric_limits<double>::max();
};

class TileIncrementalFrontier {
public:
    // 进入某子树 visit 前的计数/队列长度快照。
    struct Snapshot {
        std::size_t renderedSize = 0;
        std::size_t loadSize = 0;
        TileSelectionCounters counters;
    };

    // 每帧遍历开始:把上帧缓存移入 prev_(供 frame-over-frame 稳定性对比 /
    // 后续 Layer 的剪枝),清空 current。
    void beginFrame() {
        prev_ = std::move(cache_);
        cache_.clear();
    }

    Snapshot snapshot(const TilePlan& plan,
                      const TileLoadQueue& loadQueue,
                      const TileSelectionCounters& counters) const {
        return Snapshot{
            plan.visibleTiles.size(),
            loadQueue.size(),
            counters};
    }

    // 子树 visit 退出时记录其净贡献。`tile` 用于按 key 存储 + 聚合子代 minMargin。
    void record(const TilesetTile& tile,
                const Snapshot& snap,
                const TilePlan& plan,
                const TileLoadQueue& loadQueue,
                const TileSelectionCounters& counters,
                const TileTraversalDetails& details,
                double tileMargin) {
        SubtreeSelectionCache entry;
        const auto& visible = plan.visibleTiles;
        if (snap.renderedSize <= visible.size()) {
            entry.rendered.assign(
                visible.begin() +
                    static_cast<std::ptrdiff_t>(snap.renderedSize),
                visible.end());
        }
        const auto& requests = loadQueue.requests();
        if (snap.loadSize <= requests.size()) {
            entry.loads.assign(
                requests.begin() +
                    static_cast<std::ptrdiff_t>(snap.loadSize),
                requests.end());
        }
        entry.counterDelta = subtract(counters, snap.counters);
        entry.details = details;
        entry.minMargin = tileMargin;
        for (const TilesetTile* child : tile.children) {
            if (!child) continue;
            if (const SubtreeSelectionCache* childEntry = find(child->key)) {
                entry.minMargin =
                    std::min(entry.minMargin, childEntry->minMargin);
            }
        }
        cache_[tile.key] = std::move(entry);
    }

    const SubtreeSelectionCache* find(const TileKey& key) const {
        const auto it = cache_.find(key);
        return it == cache_.end() ? nullptr : &it->second;
    }

    const SubtreeSelectionCache* findPrev(const TileKey& key) const {
        const auto it = prev_.find(key);
        return it == prev_.end() ? nullptr : &it->second;
    }

    std::size_t size() const { return cache_.size(); }

    // Layer 2 机会测量:本帧有多少子树的净贡献与上帧逐位相同(= L3 剪枝时可跳过
    // 重算的子树数)。这是 ③ 的天花板信号——拖动时该数越高,增量收益越大。
    // 注:frame-over-frame 稳定 ≠ 一定可安全剪枝(还需 dirty 谓词),但它给出
    // 可剪枝子树的上界。
    std::size_t stableSubtreeCount() const {
        std::size_t stable = 0;
        for (const auto& [key, entry] : cache_) {
            const SubtreeSelectionCache* previous = findPrev(key);
            if (previous && contributionEqual(entry, *previous)) {
                ++stable;
            }
        }
        return stable;
    }

    static bool contributionEqual(const SubtreeSelectionCache& a,
                                  const SubtreeSelectionCache& b) {
        return a.rendered == b.rendered &&
               loadsEqual(a.loads, b.loads) &&
               countersEqual(a.counterDelta, b.counterDelta) &&
               detailsEqual(a.details, b.details);
    }

private:
    static bool loadsEqual(const std::vector<TileLoadRequest>& a,
                           const std::vector<TileLoadRequest>& b) {
        if (a.size() != b.size()) return false;
        for (std::size_t i = 0; i < a.size(); ++i) {
            if (!(a[i].key == b[i].key) || a[i].group != b[i].group ||
                a[i].priority != b[i].priority) {
                return false;
            }
        }
        return true;
    }

    static bool countersEqual(const TileSelectionCounters& a,
                              const TileSelectionCounters& b) {
        return a.visited == b.visited && a.culled == b.culled &&
               a.kicked == b.kicked && a.fogCulled == b.fogCulled &&
               a.notYetRenderable == b.notYetRenderable &&
               a.culledVisited == b.culledVisited && a.occluded == b.occluded &&
               a.waitingForOcclusionResults == b.waitingForOcclusionResults;
    }

    static bool detailsEqual(const TileTraversalDetails& a,
                             const TileTraversalDetails& b) {
        return a.allAreRenderable == b.allAreRenderable &&
               a.anyWereRenderedLastFrame == b.anyWereRenderedLastFrame &&
               a.notYetRenderableCount == b.notYetRenderableCount;
    }

    static TileSelectionCounters subtract(const TileSelectionCounters& a,
                                          const TileSelectionCounters& b) {
        TileSelectionCounters d;
        d.visited = a.visited - b.visited;
        d.culled = a.culled - b.culled;
        d.kicked = a.kicked - b.kicked;
        d.fogCulled = a.fogCulled - b.fogCulled;
        d.notYetRenderable = a.notYetRenderable - b.notYetRenderable;
        d.culledVisited = a.culledVisited - b.culledVisited;
        d.occluded = a.occluded - b.occluded;
        d.waitingForOcclusionResults =
            a.waitingForOcclusionResults - b.waitingForOcclusionResults;
        return d;
    }

    std::unordered_map<TileKey, SubtreeSelectionCache> cache_;
    std::unordered_map<TileKey, SubtreeSelectionCache> prev_;
};

} // namespace earth_engine
