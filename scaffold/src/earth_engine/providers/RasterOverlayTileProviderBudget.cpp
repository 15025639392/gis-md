// I-P4: RasterOverlayTileProvider 状态预算簿记(第三刀,从 RasterOverlayTileProvider.cpp 逐字拆出)
// 内容=pending-upload 字节记账 + source-depot 预算/LRU + backpressure。全为静态
// 成员函数定义,只依赖头文件内完整类型(ProviderAsyncState/RetiredAsyncResources)
// 与 RasterOverlayImageCompositing.h 的两个共享自由函数,无匿名 helper 依赖。
// 行为逐字等价是硬约束:与拆出前逐字符一致,不夹带任何改动。

#include "RasterOverlayTileProvider.h"
#include "RasterOverlayImageCompositing.h"

#include <algorithm>
#include <deque>
#include <iterator>
#include <tuple>
#include <utility>
#include <vector>

namespace earth_engine {

int64_t RasterOverlayTileProvider::pendingUploadSizeBytes(
    const PendingUpload& upload) {
    if (upload.image) {
        return decodedImageSizeBytes(*upload.image);
    }
    if (upload.sharedImage) {
        return decodedImageSizeBytes(*upload.sharedImage);
    }
    return 0;
}

void RasterOverlayTileProvider::retainPendingUploadImageBytesLocked(
    ProviderAsyncState& state,
    const PendingUpload& upload) {
    if (upload.image) {
        state.pendingUploadBytes += decodedImageSizeBytes(*upload.image);
        trackPeakBytes(
            state.pendingUploadBytes,
            state.peakPendingUploadBytes);
        trackPendingUploadBudgetPeakLocked(state);
        return;
    }
    if (!upload.sharedImage) {
        return;
    }
    const DecodedImage* imageKey = upload.sharedImage.get();
    auto& refs = state.sharedRasterImageRefs[imageKey];
    if (refs.sizeBytes <= 0) {
        refs.sizeBytes = decodedImageSizeBytes(*upload.sharedImage);
    }
    if (refs.pendingUploadRefs == 0 && refs.sourceCacheRefs == 0) {
        state.pendingUploadBytes += refs.sizeBytes;
        trackPeakBytes(
            state.pendingUploadBytes,
            state.peakPendingUploadBytes);
    }
    if (refs.pendingUploadRefs == 0 && refs.sourceCacheRefs > 0) {
        state.pinnedSharedPendingUploadBytes += refs.sizeBytes;
    }
    ++refs.pendingUploadRefs;
    trackPendingUploadBudgetPeakLocked(state);
}

void RasterOverlayTileProvider::releasePendingUploadImageBytesLocked(
    ProviderAsyncState& state,
    const PendingUpload& upload) {
    if (upload.image) {
        releaseOwnedPendingUploadImageBytesLocked(
            state,
            decodedImageSizeBytes(*upload.image));
        return;
    }
    if (!upload.sharedImage) {
        return;
    }
    auto it = state.sharedRasterImageRefs.find(upload.sharedImage.get());
    if (it == state.sharedRasterImageRefs.end()) {
        return;
    }
    auto& refs = it->second;
    if (refs.pendingUploadRefs == 1 && refs.sourceCacheRefs > 0) {
        state.pinnedSharedPendingUploadBytes = std::max<int64_t>(
            0,
            state.pinnedSharedPendingUploadBytes - refs.sizeBytes);
    }
    if (refs.pendingUploadRefs > 0) {
        --refs.pendingUploadRefs;
    }
    if (refs.pendingUploadRefs == 0 && refs.sourceCacheRefs == 0) {
        state.pendingUploadBytes = std::max<int64_t>(
            0,
            state.pendingUploadBytes - refs.sizeBytes);
        state.sharedRasterImageRefs.erase(it);
    }
    trackPendingUploadBudgetPeakLocked(state);
}

void RasterOverlayTileProvider::releaseOwnedPendingUploadImageBytesLocked(
    ProviderAsyncState& state,
    int64_t imageBytes) {
    if (imageBytes <= 0) {
        return;
    }
    state.pendingUploadBytes = std::max<int64_t>(
        0,
        state.pendingUploadBytes - imageBytes);
    updatePendingUploadBackpressureLocked(state);
}

void RasterOverlayTileProvider::retainSourceCacheImageBytesLocked(
    ProviderAsyncState& state,
    const std::shared_ptr<const DecodedImage>& image) {
    if (!image) {
        return;
    }
    const DecodedImage* imageKey = image.get();
    auto& refs = state.sharedRasterImageRefs[imageKey];
    if (refs.sizeBytes <= 0) {
        refs.sizeBytes = decodedImageSizeBytes(*image);
    }
    if (refs.sourceCacheRefs == 0) {
        state.sourceTileDepotCacheBytes += refs.sizeBytes;
        trackPeakBytes(
            state.sourceTileDepotCacheBytes,
            state.peakSourceTileDepotCacheBytes);
        if (refs.pendingUploadRefs > 0) {
            state.pendingUploadBytes = std::max<int64_t>(
                0,
                state.pendingUploadBytes - refs.sizeBytes);
            state.pinnedSharedPendingUploadBytes += refs.sizeBytes;
        }
    }
    ++refs.sourceCacheRefs;
    trackPendingUploadBudgetPeakLocked(state);
}

void RasterOverlayTileProvider::releaseSourceCacheImageBytesLocked(
    ProviderAsyncState& state,
    const std::shared_ptr<const DecodedImage>& image) {
    if (!image) {
        return;
    }
    const DecodedImage* imageKey = image.get();
    auto it = state.sharedRasterImageRefs.find(imageKey);
    if (it == state.sharedRasterImageRefs.end()) {
        return;
    }
    auto& refs = it->second;
    if (refs.sourceCacheRefs > 0) {
        --refs.sourceCacheRefs;
    }
    if (refs.sourceCacheRefs == 0) {
        state.sourceTileDepotCacheBytes = std::max<int64_t>(
            0,
            state.sourceTileDepotCacheBytes - refs.sizeBytes);
        if (refs.pendingUploadRefs > 0) {
            state.pinnedSharedPendingUploadBytes = std::max<int64_t>(
                0,
                state.pinnedSharedPendingUploadBytes - refs.sizeBytes);
            state.pendingUploadBytes += refs.sizeBytes;
            trackPeakBytes(
                state.pendingUploadBytes,
                state.peakPendingUploadBytes);
        }
    }
    if (refs.sourceCacheRefs == 0 && refs.pendingUploadRefs == 0) {
        state.sharedRasterImageRefs.erase(it);
    }
    trackPendingUploadBudgetPeakLocked(state);
}

void RasterOverlayTileProvider::clearPendingUploadsLocked(
    ProviderAsyncState& state,
    RetiredAsyncResources& retired) {
    retired.pendingUploads.reserve(
        retired.pendingUploads.size() + state.pendingUploads.size());
    for (PendingUpload& upload : state.pendingUploads) {
        releasePendingUploadImageBytesLocked(state, upload);
        retired.pendingUploads.push_back(std::move(upload));
    }
    state.pendingUploads.clear();
    trackPendingUploadBudgetPeakLocked(state);
    enforceSourceDepotBudgetLocked(state, retired);
}

void RasterOverlayTileProvider::trackPendingUploadBudgetPeakLocked(
    ProviderAsyncState& state) {
    trackPeakBytes(
        state.pendingUploadBytes + state.pinnedSharedPendingUploadBytes,
        state.peakPendingUploadBudgetBytes);
    updatePendingUploadBackpressureLocked(state);
}

void RasterOverlayTileProvider::updatePendingUploadBackpressureLocked(
    ProviderAsyncState& state) {
    const bool active =
        state.subTileCacheBytes > 0 &&
        state.pendingUploadBytes +
                state.pinnedSharedPendingUploadBytes >=
            state.subTileCacheBytes;
    state.pendingUploadBackpressure.store(
        active,
        std::memory_order_release);
}

void RasterOverlayTileProvider::clearSourceDepotCacheLocked(
    ProviderAsyncState& state,
    RetiredAsyncResources& retired) {
    retired.sourceAssets.reserve(
        retired.sourceAssets.size() + state.sourceTileDepotCache.size());
    for (auto& [_, asset] : state.sourceTileDepotCache) {
        if (asset.image) {
            releaseSourceCacheImageBytesLocked(state, asset.image);
        }
        retired.sourceAssets.push_back(std::move(asset));
    }
    state.sourceTileDepotCache.clear();
    state.sourceTileDepotCacheLru.clear();
    state.sourceTileDepotCacheBytes = 0;
}

void RasterOverlayTileProvider::enforceSourceDepotBudgetLocked(
    ProviderAsyncState& state,
    RetiredAsyncResources& retired) {
    const int64_t totalBudgetBytes = std::max<int64_t>(0, state.subTileCacheBytes);
    const int64_t sourceBudgetBytes = std::max<int64_t>(
        0,
        totalBudgetBytes - state.pendingUploadBytes);
    while (state.sourceTileDepotCacheBytes > sourceBudgetBytes &&
           !state.sourceTileDepotCacheLru.empty()) {
        auto [key, generation] = state.sourceTileDepotCacheLru.front();
        state.sourceTileDepotCacheLru.pop_front();
        auto it = state.sourceTileDepotCache.find(key);
        if (it == state.sourceTileDepotCache.end() ||
            it->second.generation != generation) {
            continue;
        }
        if (it->second.image) {
            releaseSourceCacheImageBytesLocked(state, it->second.image);
        } else {
            state.sourceTileDepotCacheBytes -= it->second.sizeBytes;
        }
        retired.sourceAssets.push_back(std::move(it->second));
        state.sourceTileDepotCache.erase(it);
    }
    if (state.sourceTileDepotCacheBytes < 0) {
        state.sourceTileDepotCacheBytes = 0;
    }
    compactSourceDepotCacheLruLocked(state);
}

void RasterOverlayTileProvider::clearSourceDepotInFlightLocked(
    ProviderAsyncState& state,
    RetiredAsyncResources& retired) {
    retired.inFlightSources.reserve(
        retired.inFlightSources.size() +
        state.sourceTileDepotInFlight.size());
    for (auto& [_, source] : state.sourceTileDepotInFlight) {
        retired.inFlightSources.push_back(std::move(source));
    }
    state.sourceTileDepotInFlight.clear();
}

void RasterOverlayTileProvider::compactSourceDepotCacheLruLocked(
    ProviderAsyncState& state) {
    constexpr size_t kLruSlackEntries = 32;
    const size_t liveEntries = state.sourceTileDepotCache.size();
    if (state.sourceTileDepotCacheLru.size() <=
        liveEntries + kLruSlackEntries) {
        return;
    }
    if (liveEntries > 0 &&
        state.sourceTileDepotCacheLru.size() <=
            liveEntries * 2 + kLruSlackEntries) {
        return;
    }

    std::vector<std::pair<std::string, uint64_t>> compactedEntries;
    compactedEntries.reserve(liveEntries);
    for (const auto& [key, source] : state.sourceTileDepotCache) {
        compactedEntries.emplace_back(key, source.generation);
    }
    std::sort(
        compactedEntries.begin(),
        compactedEntries.end(),
        [](const auto& left, const auto& right) {
            return std::tie(left.second, left.first) <
                   std::tie(right.second, right.first);
        });

    std::deque<std::pair<std::string, uint64_t>> compactedLru;
    compactedLru.insert(
        compactedLru.end(),
        std::make_move_iterator(compactedEntries.begin()),
        std::make_move_iterator(compactedEntries.end()));
    state.sourceTileDepotCacheLru.swap(compactedLru);
}

void RasterOverlayTileProvider::compactActiveMappedSourceSetOrderLocked(
    ProviderAsyncState& state) {
    if (state.activeMappedSourceSetOrder.empty()) {
        return;
    }
    std::deque<std::string> compactedOrder;
    for (const std::string& cacheKey : state.activeMappedSourceSetOrder) {
        auto it = state.activeMappedSourceSets.find(cacheKey);
        if (it == state.activeMappedSourceSets.end() || !it->second) {
            continue;
        }
        compactedOrder.push_back(cacheKey);
    }
    state.activeMappedSourceSetOrder.swap(compactedOrder);
}

} // namespace earth_engine
