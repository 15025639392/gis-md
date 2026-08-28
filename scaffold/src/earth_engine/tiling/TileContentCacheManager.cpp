#include "DirectRasterMapping.h"
#include "TileBaseCoveragePin.h"
#include "TileContentCacheManager.h"

namespace earth_engine {

void TileContentCacheManager::updateTotalBytesUsed(
    const std::unordered_map<std::string, std::unique_ptr<TilesetTile>>& tiles,
    const TileContentLifecycleManager& /*lifecycle*/) {
    totalBytesUsed_ = 0;
    tileBytes_.clear();
    for (const auto& [cacheKey, tile] : tiles) {
        if (!tile) {
            continue;
        }
        const int64_t bytes = std::max<int64_t>(
            0,
            tile->content.renderContent.estimateRetainedBytes());
        tileBytes_.emplace(
            cacheKey,
            TileByteAccount{
                tile->content.renderContent.retainedResourcesRevision(),
                bytes});
        totalBytesUsed_ += bytes;
    }
}

bool TileContentCacheManager::reconcileTileBytes(const TilesetTile& tile) {
    const std::string cacheKey = TileCacheKey::forTile(tile.key);
    const uint64_t retainedResourcesRevision =
        tile.content.renderContent.retainedResourcesRevision();
    const auto existing = tileBytes_.find(cacheKey);
    if (existing != tileBytes_.end() &&
        existing->second.retainedResourcesRevision ==
            retainedResourcesRevision) {
        return false;
    }
    const int64_t currentBytes =
        std::max<int64_t>(
            0,
            tile.content.renderContent.estimateRetainedBytes());
    const int64_t previousBytes =
        existing == tileBytes_.end() ? 0 : existing->second.bytes;

    totalBytesUsed_ =
        std::max<int64_t>(0, totalBytesUsed_ - previousBytes + currentBytes);
    tileBytes_[cacheKey] =
        TileByteAccount{retainedResourcesRevision, currentBytes};
    return true;
}

void TileContentCacheManager::eraseTileBytes(
    const std::string& cacheKey) {
    const auto existing = tileBytes_.find(cacheKey);
    if (existing == tileBytes_.end()) {
        return;
    }
    totalBytesUsed_ =
        std::max<int64_t>(0, totalBytesUsed_ - existing->second.bytes);
    tileBytes_.erase(existing);
    pendingProtectedUnloads_.erase(cacheKey);
}

int64_t TileContentCacheManager::accountedTileBytes(
    const std::string& cacheKey) const {
    const auto existing = tileBytes_.find(cacheKey);
    return existing == tileBytes_.end() ? 0 : existing->second.bytes;
}

void TileContentCacheManager::markEligibleForUnloading(
    const TilesetTile* tile,
    const std::string& cacheKey) {
    // 根层常驻:兜底层永不入预算驱逐队列(本函数是唯一入队口)。用
    // markIneligible 而非静默返回 —— 早先入队的同键残留也要清掉。
    if (baseCoveragePinned_ && tile &&
        isBaseCoveragePinnedZoom(tile->key.z)) {
        markIneligibleForUnloading(cacheKey);
        return;
    }
    TileIndexState::markEligibleForUnloading(unloadQueue_, tile, cacheKey);
}

void TileContentCacheManager::markIneligibleForUnloading(
    const std::string& cacheKey) {
    TileIndexState::markIneligibleForUnloading(unloadQueue_, cacheKey);
}

void TileContentCacheManager::eraseTileIndexState(
    const std::string& cacheKey,
    TileContentLifecycleManager& lifecycle,
    TileLoadQueue& loadQueue) {
    eraseTileBytes(cacheKey);
    pendingProtectedUnloads_.erase(cacheKey);
    TileIndexState::eraseCacheKeyState(
        cacheKey,
        unloadQueue_,
        nullptr,
        lifecycle.emptyContentRegistry(),
        loadQueue,
        lifecycle.loadLifecycle(),
        [](const TileKey& tileKey) {
            return TileCacheKey::forTile(tileKey);
        });
}

TileCacheUnloadContentResult TileContentCacheManager::unloadTileContent(
    TilesetTile& tile,
    TileContentLifecycleManager& lifecycle,
    IPrepareRendererResources* pPrepRenderer) {
    const std::string cacheKey = TileCacheKey::forTile(tile.key);
    const TileCacheUnloadContentResult result =
        TileContentUnloadCoordinator::unloadContent(
        tile,
        cacheKey,
        nullptr,
        lifecycle.emptyContentRegistry(),
        pPrepRenderer);
    reconcileTileBytes(tile);
    if (result == TileCacheUnloadContentResult::Keep &&
        tile.content.loadState == TileLoadState::Unloading) {
        pendingProtectedUnloads_.insert(cacheKey);
    } else {
        pendingProtectedUnloads_.erase(cacheKey);
    }
    return result;
}

} // namespace earth_engine
