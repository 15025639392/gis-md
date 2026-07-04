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
    PendingLoadFinalizeContext context) {
    if (uploads_.ordered.empty()) {
        return std::nullopt;
    }
    // Urgent 组排序在最前:交互期若队首都不是 Urgent,则全队列皆非。
    auto bestIt = uploads_.ordered.begin();
    if (context.interactionActive &&
        bestIt->first.group != TileLoadPriorityGroup::Urgent) {
        return std::nullopt;
    }
    if (!context.budget.tryFinalize(
            uploadLaneForDomain(bestIt->second.domain),
            TileLoadPriorityPolicy::toFramePriority(bestIt->first.group))) {
        return std::nullopt;
    }
    return take(uploads_, bestIt);
}

std::optional<PendingTileLoad>
TilePendingLoadQueue::takeHighestPriorityUpload(
    bool interactionActive,
    FrameResourceBudget& budget) {
    return takeHighestPriorityUpload(
        PendingLoadFinalizeContext{interactionActive, budget});
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
