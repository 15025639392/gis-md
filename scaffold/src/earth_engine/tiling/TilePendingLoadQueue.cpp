#include "TilePendingLoadQueue.h"

#include "TileLoadPriorityPolicy.h"

#include <algorithm>
#include <utility>

namespace earth_engine {

namespace {

FrameResourceLane uploadLaneForDomain(TileLoadDomain domain) {
    return isContentLoadDomain(domain)
               ? FrameResourceLane::ContentFinalize
               : FrameResourceLane::TerrainFinalize;
}

} // namespace

void TilePendingLoadQueue::IndexedLoads::insert(PendingTileLoad load) {
    const PriorityKey key{load.group, load.priority};
    std::string cacheKey = load.cacheKey;
    auto it = ordered.emplace(key, std::move(load));
    byKey[std::move(cacheKey)] = it;
}

void TilePendingLoadQueue::IndexedLoads::eraseKey(
    const std::string& cacheKey) {
    auto indexIt = byKey.find(cacheKey);
    if (indexIt == byKey.end()) {
        return;
    }
    ordered.erase(indexIt->second);
    byKey.erase(indexIt);
}

void TilePendingLoadQueue::IndexedLoads::clear() {
    ordered.clear();
    byKey.clear();
}

bool TilePendingLoadQueue::containsCacheKey(
    const std::string& cacheKey) const {
    if (cacheKey.empty()) {
        return false;
    }
    return uploadKeys_.count(cacheKey) > 0 ||
           uploads_.byKey.count(cacheKey) > 0 ||
           terminalResults_.byKey.count(cacheKey) > 0;
}

void TilePendingLoadQueue::addUpload(PendingTileLoad upload) {
    if (upload.cacheKey.empty()) {
        return;
    }
    if (terminalResults_.byKey.count(upload.cacheKey)) {
        return;
    }
    if (!uploadKeys_.insert(upload.cacheKey).second) {
        return;
    }
    uploads_.insert(std::move(upload));
}

void TilePendingLoadQueue::addTerminalResult(PendingTileLoad result) {
    if (result.cacheKey.empty()) {
        return;
    }
    if (uploadKeys_.count(result.cacheKey)) {
        return;
    }
    if (terminalResults_.byKey.count(result.cacheKey)) {
        return;
    }
    terminalResults_.insert(std::move(result));
}

void TilePendingLoadQueue::eraseUploadKey(
    const std::string& cacheKey) {
    uploadKeys_.erase(cacheKey);
}

void TilePendingLoadQueue::eraseCacheKey(const std::string& cacheKey) {
    uploadKeys_.erase(cacheKey);
    uploads_.eraseKey(cacheKey);
    terminalResults_.eraseKey(cacheKey);
}

void TilePendingLoadQueue::clear() {
    uploadKeys_.clear();
    uploads_.clear();
    terminalResults_.clear();
}

bool TilePendingLoadQueue::hasWork() const {
    return !uploadKeys_.empty() ||
           !uploads_.ordered.empty() ||
           !terminalResults_.ordered.empty();
}

size_t TilePendingLoadQueue::uploadCount() const {
    return uploads_.ordered.size();
}

size_t TilePendingLoadQueue::terminalResultCount() const {
    return terminalResults_.ordered.size();
}

size_t TilePendingLoadQueue::gltfTerrainUploadCount() const {
    return countDomain(uploads_.ordered, TileLoadDomain::TerrainContent);
}

size_t TilePendingLoadQueue::gltfTerrainTerminalResultCount() const {
    return countDomain(terminalResults_.ordered,
                       TileLoadDomain::TerrainContent);
}

size_t TilePendingLoadQueue::contentUploadCount() const {
    return countDomain(uploads_.ordered, TileLoadDomain::Content);
}

size_t TilePendingLoadQueue::contentTerminalResultCount() const {
    return countDomain(terminalResults_.ordered, TileLoadDomain::Content);
}

std::optional<PendingTileLoad>
TilePendingLoadQueue::takeHighestPriorityTerminalResult(
    FrameResourceBudget& budget) {
    if (terminalResults_.ordered.empty()) {
        return std::nullopt;
    }
    auto bestIt = terminalResults_.ordered.begin();
    if (!budget.tryFinalize(
            FrameResourceLane::TerminalState,
            TileLoadPriorityPolicy::toFramePriority(bestIt->first.group))) {
        return std::nullopt;
    }
    return take(terminalResults_, bestIt);
}

std::optional<PendingTileLoad>
TilePendingLoadQueue::takeHighestPriorityUpload(
    FrameResourceBudget& budget) {
    if (uploads_.ordered.empty()) {
        return std::nullopt;
    }
    // 交互期节奏**只由 budget lane 限速**(平滑期 4ms → 2 次/帧),不再按优先级
    // 组硬冻结。此前这里有一条 "interactionActive && 队首非 Urgent → 早退";
    // 早退发生在 tryFinalize **之前**,所以交互期预算使用次数恒为 0,
    // Preload/Normal 的上传全量积压(实测积压随交互时长线性涨到 48,停手瞬间
    // 一次性泄出、追赶 0.53s = "停手后暂态"的成因)。
    // 影像侧同样的硬冻结早已改成"只按单次上传成本过滤 + lane 涓流"
    // (RasterOverlayTileProvider.cpp:88 注释记录了这次改造),地形侧此前未跟进。
    // Urgent 仍排在最前 → 近景可见瓦片依旧优先拿到额度,只是不再独占。
    // 判据与 A/B 台见 tools/load_ab/。
    auto bestIt = uploads_.ordered.begin();
    if (!budget.tryFinalize(
            uploadLaneForDomain(bestIt->second.domain),
            TileLoadPriorityPolicy::toFramePriority(bestIt->first.group))) {
        return std::nullopt;
    }
    return take(uploads_, bestIt);
}

size_t TilePendingLoadQueue::countDomain(
    const OrderedLoads& loads,
    TileLoadDomain domain) {
    return static_cast<size_t>(std::count_if(
        loads.begin(),
        loads.end(),
        [domain](const OrderedLoads::value_type& entry) {
            return entry.second.domain == domain;
        }));
}

std::optional<PendingTileLoad> TilePendingLoadQueue::take(
    IndexedLoads& loads,
    OrderedLoads::iterator it) {
    std::optional<PendingTileLoad> result{std::move(it->second)};
    loads.byKey.erase(result->cacheKey);
    loads.ordered.erase(it);
    return result;
}

} // namespace earth_engine
