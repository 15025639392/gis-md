#include "TilePendingLoadQueue.h"

#include "TileLoadPriorityPolicy.h"

#include <algorithm>

namespace earth_engine {

namespace {

FrameResourceLane uploadLaneForDomain(TileLoadDomain domain) {
    return domain == TileLoadDomain::Content
               ? FrameResourceLane::ContentFinalize
               : FrameResourceLane::TerrainFinalize;
}

} // namespace

bool TilePendingLoadQueue::containsCacheKey(
    const std::string& cacheKey) const {
    if (cacheKey.empty()) {
        return false;
    }
    if (uploadKeys_.count(cacheKey)) {
        return true;
    }
    const auto uploadIt = std::find_if(
        uploads_.begin(),
        uploads_.end(),
        [&cacheKey](const PendingTileLoad& load) {
            return load.cacheKey == cacheKey;
        });
    if (uploadIt != uploads_.end()) {
        return true;
    }
    const auto terminalIt = std::find_if(
        terminalResults_.begin(),
        terminalResults_.end(),
        [&cacheKey](const PendingTileLoad& load) {
            return load.cacheKey == cacheKey;
        });
    return terminalIt != terminalResults_.end();
}

void TilePendingLoadQueue::addUpload(PendingTileLoad upload) {
    if (upload.cacheKey.empty()) {
        return;
    }
    const auto terminalIt = std::find_if(
        terminalResults_.begin(),
        terminalResults_.end(),
        [&upload](const PendingTileLoad& pending) {
            return pending.cacheKey == upload.cacheKey;
        });
    if (terminalIt != terminalResults_.end()) {
        return;
    }
    if (!uploadKeys_.insert(upload.cacheKey).second) {
        return;
    }
    uploads_.push_back(std::move(upload));
}

void TilePendingLoadQueue::addTerminalResult(PendingTileLoad result) {
    if (result.cacheKey.empty()) {
        return;
    }
    if (uploadKeys_.count(result.cacheKey)) {
        return;
    }
    const auto existingIt = std::find_if(
        terminalResults_.begin(),
        terminalResults_.end(),
        [&result](const PendingTileLoad& pending) {
            return pending.cacheKey == result.cacheKey;
        });
    if (existingIt != terminalResults_.end()) {
        return;
    }
    terminalResults_.push_back(std::move(result));
}

void TilePendingLoadQueue::eraseUploadKey(
    const std::string& cacheKey) {
    uploadKeys_.erase(cacheKey);
}

void TilePendingLoadQueue::eraseCacheKey(const std::string& cacheKey) {
    uploadKeys_.erase(cacheKey);
    uploads_.erase(
        std::remove_if(
            uploads_.begin(),
            uploads_.end(),
            [&cacheKey](const PendingTileLoad& load) {
                return load.cacheKey == cacheKey;
            }),
        uploads_.end());
    terminalResults_.erase(
        std::remove_if(
            terminalResults_.begin(),
            terminalResults_.end(),
            [&cacheKey](const PendingTileLoad& load) {
                return load.cacheKey == cacheKey;
            }),
        terminalResults_.end());
}

void TilePendingLoadQueue::clear() {
    uploadKeys_.clear();
    uploads_.clear();
    terminalResults_.clear();
}

bool TilePendingLoadQueue::hasWork() const {
    return !uploadKeys_.empty() ||
           !uploads_.empty() ||
           !terminalResults_.empty();
}

size_t TilePendingLoadQueue::uploadCount() const {
    return uploads_.size();
}

size_t TilePendingLoadQueue::terminalResultCount() const {
    return terminalResults_.size();
}

size_t TilePendingLoadQueue::terrainUploadCount() const {
    return countDomain(uploads_, TileLoadDomain::Terrain);
}

size_t TilePendingLoadQueue::terrainTerminalResultCount() const {
    return countDomain(terminalResults_, TileLoadDomain::Terrain);
}

size_t TilePendingLoadQueue::contentUploadCount() const {
    return countDomain(uploads_, TileLoadDomain::Content);
}

size_t TilePendingLoadQueue::contentTerminalResultCount() const {
    return countDomain(terminalResults_, TileLoadDomain::Content);
}

std::optional<PendingTileLoad>
TilePendingLoadQueue::takeHighestPriorityTerminalResult(
    FrameResourceBudget& budget) {
    if (terminalResults_.empty()) {
        return std::nullopt;
    }
    auto bestIt = TileLoadPriorityPolicy::selectHighestPriority(
        terminalResults_.begin(),
        terminalResults_.end());
    if (!budget.tryFinalize(
            FrameResourceLane::TerminalState,
            TileLoadPriorityPolicy::toFramePriority(bestIt->group))) {
        return std::nullopt;
    }
    std::optional<PendingTileLoad> result{std::move(*bestIt)};
    terminalResults_.erase(bestIt);
    return result;
}

std::optional<PendingTileLoad>
TilePendingLoadQueue::takeHighestPriorityUpload(
    PendingLoadFinalizeContext context) {
    auto bestIt = uploads_.end();
    for (auto it = uploads_.begin(); it != uploads_.end(); ++it) {
        if (context.interactionActive &&
            it->group != TileLoadPriorityGroup::Urgent) {
            continue;
        }
        if (bestIt == uploads_.end() ||
            TileLoadPriorityPolicy::hasHigherPriority(
                it->group,
                it->priority,
                bestIt->group,
                bestIt->priority)) {
            bestIt = it;
        }
    }
    if (bestIt == uploads_.end()) {
        return std::nullopt;
    }
    if (!context.budget.tryFinalize(
            uploadLaneForDomain(bestIt->domain),
            TileLoadPriorityPolicy::toFramePriority(bestIt->group))) {
        return std::nullopt;
    }
    std::optional<PendingTileLoad> upload{std::move(*bestIt)};
    uploads_.erase(bestIt);
    return upload;
}

std::optional<PendingTileLoad>
TilePendingLoadQueue::takeHighestPriorityUpload(
    bool interactionActive,
    FrameResourceBudget& budget) {
    return takeHighestPriorityUpload(
        PendingLoadFinalizeContext{interactionActive, budget});
}

size_t TilePendingLoadQueue::countDomain(
    const std::deque<PendingTileLoad>& loads,
    TileLoadDomain domain) {
    return static_cast<size_t>(std::count_if(
        loads.begin(),
        loads.end(),
        [domain](const PendingTileLoad& load) {
            return load.domain == domain;
        }));
}

} // namespace earth_engine
