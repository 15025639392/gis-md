#include "TilePendingLoadQueue.h"

#include "TileLoadPriorityPolicy.h"

#include <algorithm>

namespace earth_engine {

bool TilePendingLoadQueue::containsCacheKey(
    const std::string& cacheKey) const {
    if (cacheKey.empty()) {
        return false;
    }
    if (terrainUploadKeys_.count(cacheKey) ||
        contentUploadKeys_.count(cacheKey)) {
        return true;
    }
    const auto terrainUploadIt = std::find_if(
        terrainUploads_.begin(),
        terrainUploads_.end(),
        [&cacheKey](const PendingTerrainUpload& upload) {
            return upload.cacheKey == cacheKey;
        });
    if (terrainUploadIt != terrainUploads_.end()) {
        return true;
    }
    const auto terrainTerminalIt = std::find_if(
        terrainTerminalResults_.begin(),
        terrainTerminalResults_.end(),
        [&cacheKey](const PendingTerrainTerminalResult& result) {
            return result.cacheKey == cacheKey;
        });
    if (terrainTerminalIt != terrainTerminalResults_.end()) {
        return true;
    }
    const auto contentUploadIt = std::find_if(
        contentUploads_.begin(),
        contentUploads_.end(),
        [&cacheKey](const PendingContentUpload& upload) {
            return upload.cacheKey == cacheKey;
        });
    if (contentUploadIt != contentUploads_.end()) {
        return true;
    }
    const auto contentTerminalIt = std::find_if(
        contentTerminalResults_.begin(),
        contentTerminalResults_.end(),
        [&cacheKey](const PendingContentTerminalResult& result) {
            return result.cacheKey == cacheKey;
        });
    return contentTerminalIt != contentTerminalResults_.end();
}

void TilePendingLoadQueue::addTerrainUpload(PendingTerrainUpload upload) {
    if (upload.cacheKey.empty()) {
        return;
    }
    if (!terrainUploadKeys_.insert(upload.cacheKey).second) {
        return;
    }
    terrainUploads_.push_back(std::move(upload));
}

void TilePendingLoadQueue::addTerrainTerminalResult(
    PendingTerrainTerminalResult result) {
    if (result.cacheKey.empty()) {
        return;
    }
    terrainTerminalResults_.push_back(std::move(result));
}

void TilePendingLoadQueue::addContentUpload(PendingContentUpload upload) {
    if (upload.cacheKey.empty()) {
        return;
    }
    if (!contentUploadKeys_.insert(upload.cacheKey).second) {
        return;
    }
    contentUploads_.push_back(std::move(upload));
}

void TilePendingLoadQueue::addContentTerminalResult(
    PendingContentTerminalResult result) {
    if (result.cacheKey.empty()) {
        return;
    }
    contentTerminalResults_.push_back(std::move(result));
}

void TilePendingLoadQueue::eraseTerrainUploadKey(
    const std::string& cacheKey) {
    terrainUploadKeys_.erase(cacheKey);
}

void TilePendingLoadQueue::eraseContentUploadKey(
    const std::string& cacheKey) {
    contentUploadKeys_.erase(cacheKey);
}

void TilePendingLoadQueue::eraseCacheKey(const std::string& cacheKey) {
    terrainUploadKeys_.erase(cacheKey);
    contentUploadKeys_.erase(cacheKey);
    terrainUploads_.erase(
        std::remove_if(
            terrainUploads_.begin(),
            terrainUploads_.end(),
            [&cacheKey](const PendingTerrainUpload& upload) {
                return upload.cacheKey == cacheKey;
            }),
        terrainUploads_.end());
    terrainTerminalResults_.erase(
        std::remove_if(
            terrainTerminalResults_.begin(),
            terrainTerminalResults_.end(),
            [&cacheKey](const PendingTerrainTerminalResult& result) {
                return result.cacheKey == cacheKey;
            }),
        terrainTerminalResults_.end());
    contentUploads_.erase(
        std::remove_if(
            contentUploads_.begin(),
            contentUploads_.end(),
            [&cacheKey](const PendingContentUpload& upload) {
                return upload.cacheKey == cacheKey;
            }),
        contentUploads_.end());
    contentTerminalResults_.erase(
        std::remove_if(
            contentTerminalResults_.begin(),
            contentTerminalResults_.end(),
            [&cacheKey](const PendingContentTerminalResult& result) {
                return result.cacheKey == cacheKey;
            }),
        contentTerminalResults_.end());
}

void TilePendingLoadQueue::clear() {
    terrainUploadKeys_.clear();
    contentUploadKeys_.clear();
    terrainUploads_.clear();
    terrainTerminalResults_.clear();
    contentUploads_.clear();
    contentTerminalResults_.clear();
}

bool TilePendingLoadQueue::hasWork() const {
    return !terrainUploadKeys_.empty() ||
           !contentUploadKeys_.empty() ||
           !terrainUploads_.empty() ||
           !terrainTerminalResults_.empty() ||
           !contentUploads_.empty() ||
           !contentTerminalResults_.empty();
}

size_t TilePendingLoadQueue::terrainUploadCount() const {
    return terrainUploads_.size();
}

size_t TilePendingLoadQueue::terrainTerminalResultCount() const {
    return terrainTerminalResults_.size();
}

size_t TilePendingLoadQueue::contentUploadCount() const {
    return contentUploads_.size();
}

size_t TilePendingLoadQueue::contentTerminalResultCount() const {
    return contentTerminalResults_.size();
}

std::optional<PendingTerrainTerminalResult>
TilePendingLoadQueue::takeHighestPriorityTerrainTerminalResult() {
    if (terrainTerminalResults_.empty()) {
        return std::nullopt;
    }
    auto bestIt = TileLoadPriorityPolicy::selectHighestPriority(
        terrainTerminalResults_.begin(),
        terrainTerminalResults_.end());
    std::optional<PendingTerrainTerminalResult> result{
        std::move(*bestIt)};
    terrainTerminalResults_.erase(bestIt);
    return result;
}

std::optional<PendingContentTerminalResult>
TilePendingLoadQueue::takeHighestPriorityContentTerminalResult() {
    if (contentTerminalResults_.empty()) {
        return std::nullopt;
    }
    auto bestIt = TileLoadPriorityPolicy::selectHighestPriority(
        contentTerminalResults_.begin(),
        contentTerminalResults_.end());
    std::optional<PendingContentTerminalResult> result{
        std::move(*bestIt)};
    contentTerminalResults_.erase(bestIt);
    return result;
}

std::optional<PendingTerminalResult>
TilePendingLoadQueue::takeHighestPriorityTerminalResult(
    FrameResourceBudget& budget) {
    auto bestTerrainIt = terrainTerminalResults_.end();
    if (!terrainTerminalResults_.empty()) {
        bestTerrainIt = TileLoadPriorityPolicy::selectHighestPriority(
            terrainTerminalResults_.begin(),
            terrainTerminalResults_.end());
    }

    auto bestContentIt = contentTerminalResults_.end();
    if (!contentTerminalResults_.empty()) {
        bestContentIt = TileLoadPriorityPolicy::selectHighestPriority(
            contentTerminalResults_.begin(),
            contentTerminalResults_.end());
    }

    const bool useContent =
        bestContentIt != contentTerminalResults_.end() &&
        (bestTerrainIt == terrainTerminalResults_.end() ||
         TileLoadPriorityPolicy::hasHigherPriority(
             bestContentIt->group,
             bestContentIt->priority,
             bestTerrainIt->group,
             bestTerrainIt->priority));

    if (useContent) {
        if (!budget.tryFinalize(
                FrameResourceLane::TerminalState,
                TileLoadPriorityPolicy::toFramePriority(
                    bestContentIt->group))) {
            return std::nullopt;
        }
        PendingTerminalResult result;
        result.kind = PendingTerminalResultKind::Content;
        result.contentResult.emplace(std::move(*bestContentIt));
        contentTerminalResults_.erase(bestContentIt);
        return result;
    }

    if (bestTerrainIt == terrainTerminalResults_.end()) {
        return std::nullopt;
    }
    if (!budget.tryFinalize(
            FrameResourceLane::TerminalState,
            TileLoadPriorityPolicy::toFramePriority(bestTerrainIt->group))) {
        return std::nullopt;
    }
    PendingTerminalResult result;
    result.kind = PendingTerminalResultKind::Terrain;
    result.terrainResult.emplace(std::move(*bestTerrainIt));
    terrainTerminalResults_.erase(bestTerrainIt);
    return result;
}

std::optional<PendingLoadFinalize>
TilePendingLoadQueue::takeHighestPriorityUpload(
    PendingLoadFinalizeContext context) {
    auto bestTerrainIt = terrainUploads_.end();
    for (auto it = terrainUploads_.begin(); it != terrainUploads_.end(); ++it) {
        if (context.interactionActive &&
            it->group != TileLoadPriorityGroup::Urgent) {
            continue;
        }
        if (bestTerrainIt == terrainUploads_.end() ||
            TileLoadPriorityPolicy::hasHigherPriority(
                it->group,
                it->priority,
                bestTerrainIt->group,
                bestTerrainIt->priority)) {
            bestTerrainIt = it;
        }
    }

    auto bestContentIt = contentUploads_.end();
    for (auto it = contentUploads_.begin(); it != contentUploads_.end(); ++it) {
        if (context.interactionActive &&
            it->group != TileLoadPriorityGroup::Urgent) {
            continue;
        }
        if (bestContentIt == contentUploads_.end() ||
            TileLoadPriorityPolicy::hasHigherPriority(
                it->group,
                it->priority,
                bestContentIt->group,
                bestContentIt->priority)) {
            bestContentIt = it;
        }
    }

    const bool useContent =
        bestContentIt != contentUploads_.end() &&
        (bestTerrainIt == terrainUploads_.end() ||
         TileLoadPriorityPolicy::hasHigherPriority(
             bestContentIt->group,
             bestContentIt->priority,
             bestTerrainIt->group,
             bestTerrainIt->priority));
    if (useContent) {
        if (!context.budget.tryFinalize(
                FrameResourceLane::ContentFinalize,
                TileLoadPriorityPolicy::toFramePriority(
                    bestContentIt->group))) {
            return std::nullopt;
        }
        PendingLoadFinalize finalize;
        finalize.kind = PendingLoadFinalizeKind::Content;
        finalize.contentUpload.emplace(std::move(*bestContentIt));
        contentUploads_.erase(bestContentIt);
        return finalize;
    }

    if (bestTerrainIt == terrainUploads_.end()) {
        return std::nullopt;
    }
    if (!context.budget.tryFinalize(
            FrameResourceLane::TerrainFinalize,
            TileLoadPriorityPolicy::toFramePriority(bestTerrainIt->group))) {
        return std::nullopt;
    }
    PendingLoadFinalize finalize;
    finalize.kind = PendingLoadFinalizeKind::Terrain;
    finalize.terrainUpload.emplace(std::move(*bestTerrainIt));
    terrainUploads_.erase(bestTerrainIt);
    return finalize;
}

std::optional<PendingLoadFinalize>
TilePendingLoadQueue::takeHighestPriorityUpload(
    bool interactionActive,
    FrameResourceBudget& budget) {
    return takeHighestPriorityUpload(
        PendingLoadFinalizeContext{interactionActive, budget});
}

} // namespace earth_engine
