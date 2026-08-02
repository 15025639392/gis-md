#pragma once

#include "TileLoadTypes.h"
#include "../core/resources/FrameResourceBudget.h"

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace earth_engine {

/// P2-7:主线程每帧在 lifecycle 锁内按预算逐个取最高优先级项。旧实现每次
/// 全队列线性扫描(积压数百时每帧数万次比较,持锁期间阻塞工作线程投递),
/// 现改为按优先级排序的 multimap(取头 O(1)、增删 O(log N))+ cacheKey
/// 哈希索引(contains/erase O(1))。等优先级项保持插入序,与旧线性扫描的
/// "先入者优先"一致。
class TilePendingLoadQueue {
public:
    bool containsCacheKey(const std::string& cacheKey) const;

    void addUpload(PendingTileLoad upload);
    void addTerminalResult(PendingTileLoad result);

    void eraseUploadKey(const std::string& cacheKey);
    void eraseCacheKey(const std::string& cacheKey);
    void clear();

    bool hasWork() const;
    size_t uploadCount() const;
    size_t terminalResultCount() const;
    size_t gltfTerrainUploadCount() const;
    size_t gltfTerrainTerminalResultCount() const;
    size_t contentUploadCount() const;
    size_t contentTerminalResultCount() const;

    std::optional<PendingTileLoad> takeHighestPriorityTerminalResult(
        FrameResourceBudget& budget);
    std::optional<PendingTileLoad> takeHighestPriorityUpload(
        FrameResourceBudget& budget);

private:
    // 排序键与 TileLoadPriorityPolicy::hasHigherPriority 一致:group 降序、
    // 组内 priority 升序(值小者优先),begin() 即最高优先级。
    struct PriorityKey {
        TileLoadPriorityGroup group;
        double priority;
        bool operator<(const PriorityKey& rhs) const {
            if (group != rhs.group) {
                return static_cast<int>(group) > static_cast<int>(rhs.group);
            }
            return priority < rhs.priority;
        }
    };
    using OrderedLoads = std::multimap<PriorityKey, PendingTileLoad>;

    struct IndexedLoads {
        OrderedLoads ordered;
        std::unordered_map<std::string, OrderedLoads::iterator> byKey;

        void insert(PendingTileLoad load);
        void eraseKey(const std::string& cacheKey);
        void clear();
    };

    static size_t countDomain(const OrderedLoads& loads,
                              TileLoadDomain domain);
    static std::optional<PendingTileLoad> take(IndexedLoads& loads,
                                               OrderedLoads::iterator it);

    std::unordered_set<std::string> uploadKeys_;
    IndexedLoads uploads_;
    IndexedLoads terminalResults_;
};

} // namespace earth_engine
