#pragma once

#include "RasterOverlayImageCompositing.h"
#include "ImageryProvider.h"
#include "../layers/RasterOverlay.h"
#include "../core/async/AsyncSystem.h"
#include "../threading/CancellationToken.h"
#include "../platform/bridge/PlatformBridge.h"

#include <algorithm>
#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace earth_engine {

// I-P4 第二刀:depot 嵌套 struct 定义(从 RasterOverlayTileProvider.cpp 拆出)。
// 定义在头文件使主文件(成员访问)与实现可见;内容为 git 原文逐字搬运。

// ============================================================
struct RasterOverlayTileProvider::QuadtreeSourceAssetDepot
    : public std::enable_shared_from_this<QuadtreeSourceAssetDepot> {
    using SourceReady = std::function<void(RasterSourceResult&&)>;

    QuadtreeSourceAssetDepot(ImageryProvider& imageryProvider,
                             const TileScheme& tileScheme,
                             std::shared_ptr<ProviderAsyncState> asyncState,
                             int minimumSourceLevel,
                             int maximumSourceLevel)
        : provider(imageryProvider)
        , scheme(tileScheme)
        , state(std::move(asyncState))
        , cache(state->sourceTileDepotCache)
        , cacheLru(state->sourceTileDepotCacheLru)
        , cacheBytes(state->sourceTileDepotCacheBytes)
        , cacheGeneration(state->sourceTileDepotGeneration)
        , depotEpoch(state->sourceTileDepotEpoch)
        , inFlight(state->sourceTileDepotInFlight)
        , cacheMutex(state->mutex)
        , minimumLevel(minimumSourceLevel)
        , maximumLevel(maximumSourceLevel) {}

    void requestSource(
        const TileKey& requestedKey,
        const TileKey& originalKey,
        bool ancestorFallback,
        bool shareInFlight,
        uint64_t waiterOwnerToken,
        const std::function<void()>& onSourceIssued,
        const std::function<void()>& onSourceFinished,
        const std::function<void()>& onSourceFailed,
        SourceReady onReady,
        std::vector<TileKey> fallbackInFlightKeys = {},
        std::function<bool()> tryAdmitSource = {},
        std::function<void()> onSourceAdmissionDenied = {}) {
        if (waiterOwnerToken != 0) {
            std::lock_guard<std::mutex> lock(cacheMutex);
            if (state->activeMappedSourceOwnerTokens.count(waiterOwnerToken) ==
                0) {
                return;
            }
        }
        // 值捕获(teardown 竞态根修,tombstone_21):worker 侧回调可能在
        // provider/scheme 析构后运行 —— depot 经 shared_from_this 有意续命,
        // 而 `provider`/`scheme` 是指向 overlay 世界的裸引用,不随之续命
        // (实测:退后台 132ms 后解码回调走 abandon 路径,解引用已释放的
        // scheme,空 vptr 虚调用)。回调需要的宿主数据在**发起时**(此刻宿主
        // 必活:本函数只在 provider 帧泵线程被调)全部按值算好带走,回调里
        // 不得再触碰 scheme/provider。
        const Rectangle originalBounds = scheme.tileToRectangle(originalKey);
        const Rectangle requestedBounds =
            scheme.tileToRectangle(requestedKey);
        const std::string attributionSnapshot = provider.attribution();
        std::optional<RasterSourceResult> cachedSource;
        {
            std::lock_guard<std::mutex> lock(cacheMutex);
            const std::string originalCacheKey = depotCacheKey(originalKey);
            auto it = cache.find(originalCacheKey);
            if (it == cache.end() && ancestorFallback) {
                it = cache.find(depotCacheKey(requestedKey));
            }
            if (it != cache.end() &&
                (it->second.image || it->second.terminalFailure)) {
                touchCachedSource(it->first, it->second);
                RasterSourceResult source;
                source.key = it->second.key;
                source.bounds = it->second.bounds;
                source.image = it->second.image;
                source.sourceSubset = ancestorFallback
                    ? std::optional<Rectangle>(originalBounds)
                    : it->second.sourceSubset;
                source.moreDetailAvailable = it->second.moreDetailAvailable;
                source.diagnostics = it->second.diagnostics;
                source.credits = it->second.credits;
                source.terminalFailure = it->second.terminalFailure;
                cachedSource = std::move(source);
            }
        }
        if (cachedSource) {
            if (ancestorFallback) {
                auto completed = std::make_shared<SourceTileAsset>(
                    sourceAssetFromResult(*cachedSource));
                if (finishInFlightSource(originalKey, completed) > 0) {
                    return;
                }
            }
            if (onReady) {
                onReady(std::move(*cachedSource));
            }
            return;
        }

        auto self = shared_from_this();
        bool freshTransportAdmitted = false;
        const auto admitFreshTransport = [&]() {
            if (!tryAdmitSource) {
                return true;
            }
            try {
                return tryAdmitSource();
            } catch (...) {
                return false;
            }
        };
        if (shareInFlight) {
            const std::string inFlightKey = depotCacheKey(originalKey);
            auto waiter =
                [self, originalKey, originalBounds, ancestorFallback,
                 onReady](InFlightSourceTileAsset::Result cached) mutable {
                    if (onReady) {
                        onReady(self->rasterSourceResultFromAsset(
                            cached,
                            originalKey,
                            originalBounds,
                            ancestorFallback));
                    }
                };
            {
                std::lock_guard<std::mutex> lock(cacheMutex);
                if (waiterOwnerToken != 0 &&
                    state->activeMappedSourceOwnerTokens.count(
                        waiterOwnerToken) == 0) {
                    return;
                }
                auto [it, inserted] =
                    inFlight.try_emplace(inFlightKey, InFlightSourceTileAsset{});
                it->second.waiters.push_back(InFlightSourceTileAsset::WaiterEntry{
                    waiterOwnerToken,
                    std::move(waiter)});
                if (!inserted) {
                    return;
                }
                if (!admitFreshTransport()) {
                    inFlight.erase(it);
                    if (onSourceAdmissionDenied) {
                        onSourceAdmissionDenied();
                    }
                    return;
                }
                freshTransportAdmitted = true;
            }
        }

        if (ancestorFallback) {
            const std::string fallbackInFlightKey =
                depotCacheKey(requestedKey);
            auto waiter =
                [self,
                 originalKey,
                 originalBounds,
                 ancestorFallback,
                 onReady,
                 fallbackInFlightKeys](
                    InFlightSourceTileAsset::Result cached) mutable {
                    RasterSourceResult source =
                        self->rasterSourceResultFromAsset(
                        cached,
                        originalKey,
                        originalBounds,
                        ancestorFallback);
                    if (cached) {
                        auto originalCompleted =
                            std::make_shared<SourceTileAsset>(
                                self->sourceAssetFromResult(source));
                        self->finishInFlightSource(
                            originalKey,
                            originalCompleted);
                        for (const TileKey& key : fallbackInFlightKeys) {
                            self->finishInFlightSource(key, cached);
                        }
                    } else if (onReady) {
                        onReady(std::move(source));
                    }
                };
            {
                std::lock_guard<std::mutex> lock(cacheMutex);
                if (waiterOwnerToken != 0 &&
                    state->activeMappedSourceOwnerTokens.count(
                        waiterOwnerToken) == 0) {
                    return;
                }
                auto [it, inserted] =
                    inFlight.try_emplace(
                        fallbackInFlightKey,
                        InFlightSourceTileAsset{});
                if (waiterOwnerToken != 0) {
                    auto& keys =
                        self->state->sourceTileDepotFallbackKeysByOwner[
                            waiterOwnerToken];
                    if (std::find(keys.begin(), keys.end(), requestedKey) ==
                        keys.end()) {
                        keys.push_back(requestedKey);
                    }
                }
                if (!inserted) {
                    it->second.waiters.push_back(
                        InFlightSourceTileAsset::WaiterEntry{
                            waiterOwnerToken,
                            std::move(waiter)});
                    return;
                }
                if (!freshTransportAdmitted && !admitFreshTransport()) {
                    inFlight.erase(it);
                    if (waiterOwnerToken != 0) {
                        auto ownerIt =
                            state->sourceTileDepotFallbackKeysByOwner.find(
                                waiterOwnerToken);
                        if (ownerIt !=
                            state->sourceTileDepotFallbackKeysByOwner.end()) {
                            auto& keys = ownerIt->second;
                            keys.erase(
                                std::remove(keys.begin(), keys.end(),
                                            requestedKey),
                                keys.end());
                            if (keys.empty()) {
                                state->sourceTileDepotFallbackKeysByOwner
                                    .erase(ownerIt);
                            }
                        }
                    }
                    if (onSourceAdmissionDenied) {
                        onSourceAdmissionDenied();
                    }
                    return;
                }
                freshTransportAdmitted = true;
            }
            fallbackInFlightKeys.push_back(requestedKey);
        }

        if (onSourceIssued) {
            onSourceIssued();
        }
        CancellationToken token;
        const std::vector<TileKey> exceptionInFlightKeys =
            fallbackInFlightKeys;
        auto callback =
            [self,
             requestedKey,
             originalKey,
             // 值快照:本回调在 worker 线程运行,可能晚于 provider/scheme
             // 析构 —— 回调体内禁止触碰 self->scheme / self->provider
             // (见 requestSource 顶部注释)。
             originalBounds,
             requestedBounds,
             attributionSnapshot,
             ancestorFallback,
             waiterOwnerToken,
             onSourceIssued,
             onSourceFinished,
             onSourceFailed,
             onReady,
             fallbackInFlightKeys = std::move(fallbackInFlightKeys)](
                const TileKey& loadedKey,
                std::unique_ptr<DecodedImage> image) mutable {
                if (!self->state->alive.load(std::memory_order_acquire)) {
                    if (onSourceFinished) {
                        onSourceFinished();
                    }
                    auto abandoned = self->makeAbandonedSourceResult(
                        originalKey, originalBounds);
                    self->finishInFlightSource(originalKey, abandoned);
                    for (const TileKey& key : fallbackInFlightKeys) {
                        self->finishInFlightSource(key, abandoned);
                    }
                    return;
                }

                if (image) {
                    if (onSourceFinished) {
                        onSourceFinished();
                    }
                    RasterSourceResult source;
                    source.key = loadedKey;
                    // provider 回调按契约回带 requestedKey(逐级回退各是独立
                    // 的 requestSource 实例,各带各的快照)→ 用发起时算好的
                    // requestedBounds,不回摸 scheme。
                    source.bounds = requestedBounds;
                    source.image =
                        std::shared_ptr<const DecodedImage>(std::move(image));
                    source.sourceSubset = ancestorFallback
                        ? std::optional<Rectangle>(originalBounds)
                        : std::nullopt;
                    source.moreDetailAvailable =
                        loadedKey.z < self->maximumLevel
                            ? RasterOverlayTile::MoreDetailAvailable::Yes
                            : RasterOverlayTile::MoreDetailAvailable::No;
                    if (!attributionSnapshot.empty()) {
                        source.credits.push_back(attributionSnapshot);
                    }
                    auto completed = std::make_shared<SourceTileAsset>(
                        self->sourceAssetFromResult(source));
                    InFlightSourceTileAsset::Result directCompleted =
                        completed;
                    if (!ancestorFallback) {
                        self->cacheSource(originalKey, source);
                    }
                    if (loadedKey != originalKey) {
                        RasterSourceResult directSource;
                        directSource.key = loadedKey;
                        directSource.bounds = source.bounds;
                        directSource.image = source.image;
                        directSource.sourceSubset = std::nullopt;
                        directSource.moreDetailAvailable =
                            source.moreDetailAvailable;
                        directSource.diagnostics = source.diagnostics;
                        directSource.credits = source.credits;
                        self->cacheSource(loadedKey, directSource);
                        directCompleted = std::make_shared<SourceTileAsset>(
                            self->sourceAssetFromResult(directSource));
                    }
                    self->finishInFlightSource(originalKey, completed);
                    for (const TileKey& key : fallbackInFlightKeys) {
                        self->finishInFlightSource(key, directCompleted);
                    }
                    return;
                }

                if (onSourceFailed) {
                    onSourceFailed();
                }
                if (requestedKey.z > self->minimumLevel) {
                    const TileKey parentKey = parentTileKey(requestedKey);
                    if (onSourceFinished) {
                        onSourceFinished();
                    }
                    if (!self->state->alive.load(std::memory_order_acquire)) {
                        auto abandoned = self->makeAbandonedSourceResult(
                            originalKey, originalBounds);
                        self->finishInFlightSource(originalKey, abandoned);
                        for (const TileKey& key : fallbackInFlightKeys) {
                            self->finishInFlightSource(key, abandoned);
                        }
                        return;
                    }
                    {
                        std::lock_guard<std::mutex> lock(
                            self->state->mutex);
                        self->state->pendingSourceFallbacks.push_back(
                            PendingSourceFallback{
                                originalKey,
                                parentKey,
                                waiterOwnerToken,
                                [self,
                                 parentKey,
                                 originalKey,
                                 onSourceIssued,
                                 onSourceFinished,
                                 onSourceFailed,
                                 onReady,
                                 fallbackInFlightKeys,
                                 waiterOwnerToken](
                                    std::function<bool()> tryAdmitSource)
                                    mutable {
                                    // requestSource retains onSourceIssued in
                                    // its async completion callback. A stack
                                    // reference here becomes dangling when a
                                    // failed parent queues another fallback.
                                    auto issued = std::make_shared<int>(0);
                                    auto admissionDenied =
                                        std::make_shared<std::atomic<bool>>(
                                            false);
                                    std::function<void()>
                                        onSourceAdmissionDenied =
                                            [admissionDenied]() {
                                                admissionDenied->store(
                                                    true,
                                                    std::memory_order_release);
                                            };
                                    self->requestSource(
                                        parentKey,
                                        originalKey,
                                        true,
                                        false,
                                        waiterOwnerToken,
                                        [issued, onSourceIssued]() {
                                            ++(*issued);
                                            if (onSourceIssued) {
                                                onSourceIssued();
                                            }
                                        },
                                        onSourceFinished,
                                        onSourceFailed,
                                        onReady,
                                        fallbackInFlightKeys,
                                        std::move(tryAdmitSource),
                                        std::move(onSourceAdmissionDenied));
                                    return admissionDenied->load(
                                               std::memory_order_acquire)
                                        ? -1
                                        : *issued;
                                }});
                        self->state->pendingSourceFallbackCount.store(
                            static_cast<uint32_t>(
                                self->state->pendingSourceFallbacks.size()),
                            std::memory_order_release);
                    }
                    // [2026-08-21 冻屏根修] worker 派发 fallback:确保持有
                    // Landing 票(睡死期间 worker 起的新活不能无票)。
                    RasterOverlayTileProvider::
                        syncRasterLandingTicketFromAnyThread(self->state);
                    return;
                }

                if (onSourceFinished) {
                    onSourceFinished();
                }
                auto failed = self->cacheTerminalFailure(
                    originalKey, originalBounds);
                self->finishInFlightSource(originalKey, failed);
                for (const TileKey& key : fallbackInFlightKeys) {
                    self->finishInFlightSource(key, failed);
                }
            };
        try {
            provider.requestTile(
                requestedKey,
                token,
                std::move(callback));
        } catch (...) {
            if (onSourceFailed) {
                onSourceFailed();
            }
            if (onSourceFinished) {
                onSourceFinished();
            }
            SourceTileAsset failed;
            failed.key = originalKey;
            failed.bounds = originalBounds;
            failed.moreDetailAvailable =
                RasterOverlayTile::MoreDetailAvailable::No;
            failed.diagnostics.push_back(
                "Raster source tile request threw before completion");
            failed.terminalFailure = true;
            auto transientFailure =
                std::make_shared<SourceTileAsset>(std::move(failed));
            finishInFlightSource(originalKey, transientFailure);
            for (const TileKey& key : exceptionInFlightKeys) {
                finishInFlightSource(key, transientFailure);
            }
        }
    }

    bool wouldIssueNewRequest(const TileKey& originalKey) const {
        std::lock_guard<std::mutex> lock(cacheMutex);
        const std::string key = depotCacheKey(originalKey);
        auto cached = cache.find(key);
        if (cached != cache.end() &&
            (cached->second.image || cached->second.terminalFailure)) {
            return false;
        }
        return inFlight.find(key) == inFlight.end();
    }

    void abandonInFlightSource(const TileKey& originalKey) {
        // 本方法只在 provider 帧泵线程被调(宿主必活),此处取 scheme 安全。
        finishInFlightSource(
            originalKey,
            makeAbandonedSourceResult(
                originalKey,
                scheme.tileToRectangle(originalKey)));
    }

    void detachInFlightWaiters(const std::vector<TileKey>& keys,
                               uint64_t waiterOwnerToken) {
        if (waiterOwnerToken == 0 || keys.empty()) {
            return;
        }
        std::lock_guard<std::mutex> lock(cacheMutex);
        for (const TileKey& key : keys) {
            auto it = inFlight.find(depotCacheKey(key));
            if (it == inFlight.end()) {
                continue;
            }
            auto& waiters = it->second.waiters;
            waiters.erase(
                std::remove_if(
                    waiters.begin(),
                    waiters.end(),
                    [waiterOwnerToken](
                        const InFlightSourceTileAsset::WaiterEntry& waiter) {
                        return waiter.ownerToken == waiterOwnerToken;
                }),
                waiters.end());
        }
        state->sourceTileDepotFallbackKeysByOwner.erase(waiterOwnerToken);
    }

private:
    // ⚠️ 以下三个 helper 可能在 worker 回调里、provider/scheme 析构后运行:
    // 只准消费调用方传入的值参,不得触碰 scheme/provider 成员。
    RasterSourceResult rasterSourceResultFromAsset(
        const InFlightSourceTileAsset::Result& cached,
        const TileKey& originalKey,
        const Rectangle& originalBounds,
        bool ancestorFallback) const {
        (void)originalKey;
        if (!cached) {
            return RasterSourceResult{};
        }
        RasterSourceResult source;
        source.key = cached->key;
        source.bounds = cached->bounds;
        source.image = cached->image;
        source.sourceSubset = ancestorFallback
            ? std::optional<Rectangle>(originalBounds)
            : cached->sourceSubset;
        source.moreDetailAvailable = cached->moreDetailAvailable;
        source.diagnostics = cached->diagnostics;
        source.credits = cached->credits;
        source.terminalFailure = cached->terminalFailure;
        return source;
    }

    SourceTileAsset sourceAssetFromResult(
        const RasterSourceResult& source) const {
        SourceTileAsset cached;
        cached.key = source.key;
        cached.bounds = source.bounds;
        if (source.image) {
            cached.image = source.image;
            cached.sizeBytes = decodedImageSizeBytes(*source.image);
        }
        cached.sourceSubset = source.sourceSubset;
        cached.moreDetailAvailable = source.moreDetailAvailable;
        cached.diagnostics = source.diagnostics;
        cached.credits = source.credits;
        cached.terminalFailure = source.terminalFailure;
        return cached;
    }

    InFlightSourceTileAsset::Result makeAbandonedSourceResult(
        const TileKey& requestedKey,
        const Rectangle& requestedBounds) const {
        SourceTileAsset abandoned;
        abandoned.key = requestedKey;
        abandoned.bounds = requestedBounds;
        abandoned.moreDetailAvailable =
            RasterOverlayTile::MoreDetailAvailable::No;
        abandoned.diagnostics.push_back(
            "Raster source tile abandoned after provider destruction");
        abandoned.terminalFailure = true;
        return std::make_shared<SourceTileAsset>(std::move(abandoned));
    }

    InFlightSourceTileAsset::Result cacheTerminalFailure(
        const TileKey& requestedKey,
        const Rectangle& requestedBounds) {
        SourceTileAsset failed;
        failed.key = requestedKey;
        failed.bounds = requestedBounds;
        failed.moreDetailAvailable =
            RasterOverlayTile::MoreDetailAvailable::No;
        failed.diagnostics.push_back(
            "Raster source tile failed after exhausting parent fallback");
        failed.terminalFailure = true;
        failed.sizeBytes = 1;

        auto cached = std::make_shared<SourceTileAsset>(failed);
        RetiredAsyncResources retired;
        std::unique_lock<std::mutex> lock(cacheMutex);
        if (state->sourceTileDepotEpoch != depotEpoch) {
            return cached;
        }
        const int64_t cacheBudgetBytes = std::max<int64_t>(
            0,
            state->subTileCacheBytes - state->pendingUploadBytes);
        if (cacheBudgetBytes <= 0) {
            RasterOverlayTileProvider::clearSourceDepotCacheLocked(
                *state,
                retired);
            return cached;
        }
        const std::string key = depotCacheKey(requestedKey);
        auto existing = cache.find(key);
        if (existing != cache.end()) {
            if (existing->second.image) {
                RasterOverlayTileProvider::releaseSourceCacheImageBytesLocked(
                    *state,
                    existing->second.image);
            } else {
                cacheBytes -= existing->second.sizeBytes;
            }
            retired.sourceAssets.push_back(std::move(existing->second));
            cache.erase(existing);
        }
        failed.generation = ++cacheGeneration;
        cacheBytes += failed.sizeBytes;
        trackPeakBytes(cacheBytes, state->peakSourceTileDepotCacheBytes);
        cacheLru.emplace_back(key, failed.generation);
        compactCacheLruIfNeeded();
        cache.emplace(key, std::move(failed));
        pruneCacheToBudget(cacheBudgetBytes, retired);
        return cached;
    }

    size_t finishInFlightSource(const TileKey& originalKey,
                                InFlightSourceTileAsset::Result source) {
        std::vector<InFlightSourceTileAsset::WaiterEntry> waiters;
        {
            std::unique_lock<std::mutex> lock(cacheMutex);
            auto it = inFlight.find(depotCacheKey(originalKey));
            if (it != inFlight.end()) {
                waiters = std::move(it->second.waiters);
                inFlight.erase(it);
            }
        }
        // [2026-08-21 冻屏根修] depot 源在途落地:同步 Landing 票(最后一件
        // 时释放 → 触发落地唤醒)。
        RasterOverlayTileProvider::syncRasterLandingTicketFromAnyThread(state);
        for (auto& waiter : waiters) {
            if (waiter.callback) {
                waiter.callback(source);
            }
        }
        return waiters.size();
    }

    void cacheSource(const TileKey& requestedKey,
                     const RasterSourceResult& source) {
        if (!source.image) return;
        SourceTileAsset cached = sourceAssetFromResult(source);
        RetiredAsyncResources retired;
        std::unique_lock<std::mutex> lock(cacheMutex);
        if (state->sourceTileDepotEpoch != depotEpoch) {
            return;
        }
        const int64_t cacheBudgetBytes = std::max<int64_t>(
            0,
            state->subTileCacheBytes - state->pendingUploadBytes);
        if (cacheBudgetBytes <= 0) {
            RasterOverlayTileProvider::clearSourceDepotCacheLocked(
                *state,
                retired);
            return;
        }
        const std::string key = depotCacheKey(requestedKey);
        auto existing = cache.find(key);
        if (existing != cache.end()) {
            if (existing->second.image) {
                RasterOverlayTileProvider::releaseSourceCacheImageBytesLocked(
                    *state,
                    existing->second.image);
            } else {
                cacheBytes -= existing->second.sizeBytes;
            }
            retired.sourceAssets.push_back(std::move(existing->second));
            cache.erase(existing);
        }
        cached.generation = ++cacheGeneration;
        RasterOverlayTileProvider::retainSourceCacheImageBytesLocked(
            *state,
            cached.image);
        cacheLru.emplace_back(key, cached.generation);
        compactCacheLruIfNeeded();
        cache.emplace(key, std::move(cached));
        pruneCacheToBudget(cacheBudgetBytes, retired);
    }

    void touchCachedSource(const std::string& key, SourceTileAsset& source) {
        source.generation = ++cacheGeneration;
        cacheLru.emplace_back(key, source.generation);
        compactCacheLruIfNeeded();
    }

    void pruneCacheToBudget(
        int64_t cacheBudgetBytes,
        RetiredAsyncResources& retired) {
        while (cacheBytes > cacheBudgetBytes && !cacheLru.empty()) {
            auto [key, generation] = cacheLru.front();
            cacheLru.pop_front();
            auto it = cache.find(key);
            if (it == cache.end() || it->second.generation != generation) {
                continue;
            }
            if (it->second.image) {
                RasterOverlayTileProvider::releaseSourceCacheImageBytesLocked(
                    *state,
                    it->second.image);
            } else {
                cacheBytes -= it->second.sizeBytes;
            }
            retired.sourceAssets.push_back(std::move(it->second));
            cache.erase(it);
        }
        if (cacheBytes < 0) {
            cacheBytes = 0;
        }
    }

    void compactCacheLruIfNeeded() {
        constexpr size_t kLruSlackEntries = 32;
        const size_t liveEntries = cache.size();
        if (cacheLru.size() <= liveEntries + kLruSlackEntries) {
            return;
        }
        if (liveEntries > 0 &&
            cacheLru.size() <= liveEntries * 2 + kLruSlackEntries) {
            return;
        }

        std::vector<std::pair<std::string, uint64_t>> compactedEntries;
        compactedEntries.reserve(liveEntries);
        for (const auto& [key, source] : cache) {
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
        cacheLru.swap(compactedLru);
    }

    ImageryProvider& provider;
    const TileScheme& scheme;
    std::shared_ptr<ProviderAsyncState> state;
    std::unordered_map<std::string, SourceTileAsset>& cache;
    std::deque<std::pair<std::string, uint64_t>>& cacheLru;
    int64_t& cacheBytes;
    uint64_t& cacheGeneration;
    uint64_t depotEpoch = 0;
    std::unordered_map<std::string, InFlightSourceTileAsset>& inFlight;
    std::mutex& cacheMutex;
    int minimumLevel = 0;
    int maximumLevel = 0;

    std::string depotCacheKey(const TileKey& key) const {
        return sourceCacheKey(depotEpoch, key);
    }
};

struct RasterOverlayTileProvider::MappedSourceImageSet
    : public std::enable_shared_from_this<MappedSourceImageSet> {
    MappedSourceImageSet(const TileScheme& tileScheme,
                             std::shared_ptr<ProviderAsyncState> asyncState,
                             std::shared_ptr<std::atomic<bool>>
                                 throttleSlotReleased,
                             std::shared_ptr<QuadtreeSourceAssetDepot>
                                 sourceDepot,
                             uint64_t sourceWaiterOwnerToken,
                             RasterSourceTileMapping sourceTileMapping,
                             Rectangle bounds,
                             RasterOverlayProjection outputProjection,
                             int maximumSourceLevel,
                             bool emptyWhenOnlyAncestorFallback,
                             bool allowDirectTerminalFailure,
                             MappedSourceLoadSuccess success,
                             MappedSourceLoadFailure failure)
        : scheme(createAsyncSchemeSnapshot(tileScheme))
        , state(std::move(asyncState))
        , slotReleased(std::move(throttleSlotReleased))
        , depot(std::move(sourceDepot))
        , waiterOwnerToken(sourceWaiterOwnerToken)
        , sourceTiles(std::move(sourceTileMapping))
        , targetBounds(bounds)
        , projection(outputProjection)
        , maximumLevel(maximumSourceLevel)
        , returnEmptyForAncestorOnly(emptyWhenOnlyAncestorFallback)
        , directTerminalFailure(allowDirectTerminalFailure)
        , onSuccess(std::move(success))
        , onFailure(std::move(failure))
        , remaining(sourceTiles.budgetUnits()) {
        sources.reserve(sourceTiles.sourceKeys.size());
        sourceIssued.assign(sourceTiles.sourceKeys.size(), false);
    }

    int issueSome(int maxNewRequests,
                  const std::function<bool()>& tryAdmitNewRequest,
                  const std::function<void()>& onSourceIssued,
                  const std::function<void()>& onSourceFinished,
                  const std::function<void()>& onSourceFailed) {
        auto issued = std::make_shared<int>(0);
        std::vector<std::pair<size_t, TileKey>> sourceKeys;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (completed) {
                return 0;
            }
            int remainingNewRequests = maxNewRequests;
            for (size_t i = 0; i < sourceTiles.sourceKeys.size(); ++i) {
                if (sourceIssued[i]) {
                    continue;
                }
                const TileKey& sourceKey = sourceTiles.sourceKeys[i];
                const bool needsNewRequest =
                    depot->wouldIssueNewRequest(sourceKey);
                if (needsNewRequest && remainingNewRequests <= 0) {
                    continue;
                }
                if (needsNewRequest) {
                    --remainingNewRequests;
                }
                sourceKeys.emplace_back(i, sourceKey);
                sourceIssued[i] = true;
            }
        }
        for (const auto& source : sourceKeys) {
            const size_t sourceIndex = source.first;
            const TileKey& sourceKey = source.second;
            auto self = shared_from_this();
            depot->requestSource(
                sourceKey,
                sourceKey,
                false,
                true,
                waiterOwnerToken,
                [issued, onSourceIssued]() {
                    ++(*issued);
                    onSourceIssued();
                },
                onSourceFinished,
                onSourceFailed,
                [self](RasterSourceResult&& source) {
                    self->finishOneSource(std::move(source));
                },
                {},
                tryAdmitNewRequest,
                [self, sourceIndex]() {
                    std::lock_guard<std::mutex> lock(self->mutex);
                    if (!self->completed &&
                        sourceIndex < self->sourceIssued.size()) {
                        self->sourceIssued[sourceIndex] = false;
                    }
                });
        }
        return *issued;
    }

    bool hasUnissuedSources() const {
        std::lock_guard<std::mutex> lock(mutex);
        return !completed &&
               std::any_of(
                   sourceIssued.begin(),
                   sourceIssued.end(),
                   [](bool issued) { return !issued; });
    }

    bool isComplete() const {
        std::lock_guard<std::mutex> lock(mutex);
        return completed;
    }

    void markAbandoned() {
        std::lock_guard<std::mutex> lock(mutex);
        completed = true;
        remaining = 0;
        std::fill(sourceIssued.begin(), sourceIssued.end(), true);
        sources.clear();
    }

    void releaseThrottleSlotOnce() {
        releaseRasterThrottleSlotOnce(
            *slotReleased,
            state->activeRasterTileLoads);
    }

    uint64_t getWaiterOwnerToken() const { return waiterOwnerToken; }

    const std::vector<TileKey>& getSourceKeys() const {
        return sourceTiles.sourceKeys;
    }

private:
    void finishOneSource(RasterSourceResult&& source) {
        bool finished = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (completed) {
                return;
            }
            if (isResolvedRasterSourceResult(source)) {
                sources.push_back(std::move(source));
            }
            --remaining;
            finished = remaining == 0;
            completed = finished;
        }

        if (!finished) return;

        std::vector<RasterSourceResult> completedSources;
        {
            std::lock_guard<std::mutex> lock(mutex);
            completedSources = std::move(sources);
        }

        if (directTerminalFailure &&
            completedSources.size() == 1 &&
            sourceTiles.sourceKeys.size() == 1 &&
            completedSources.front().terminalFailure &&
            !completedSources.front().image &&
            !completedSources.front().sourceSubset.has_value() &&
            rectanglesEqualForDirectRasterTile(
                targetBounds,
                completedSources.front().bounds)) {
            onFailure(std::move(completedSources.front().diagnostics));
            return;
        }

        if (completedSources.size() == 1 &&
            sourceTiles.sourceKeys.size() == 1 &&
            completedSources.front().image &&
            !completedSources.front().sourceSubset.has_value() &&
            rectanglesEqualForDirectRasterTile(
                targetBounds,
                completedSources.front().bounds)) {
            RasterSourceResult& source = completedSources.front();
            const RasterOverlayTile::MoreDetailAvailable moreDetailAvailable =
                source.moreDetailAvailable !=
                        RasterOverlayTile::MoreDetailAvailable::Unknown
                    ? source.moreDetailAvailable
                    : (source.key.z < maximumLevel
                           ? RasterOverlayTile::MoreDetailAvailable::Yes
                           : RasterOverlayTile::MoreDetailAvailable::No);
            onSuccess(
                nullptr,
                source.image,
                projectGeographicToProvider(source.bounds, projection),
                moreDetailAvailable,
                std::move(source.diagnostics),
                std::move(source.credits));
            return;
        }

        auto self = shared_from_this();
        if (!state->alive.load(std::memory_order_acquire)) {
            onFailure({});
            return;
        }
        if (!scheme) {
            onFailure({});
            return;
        }
        state->activeRasterComposeTasks.fetch_add(1, std::memory_order_relaxed);
        std::function<void()> composeTask =
            [self,
             completedSources = std::move(completedSources)]() mutable {
                bool completedTileLoad = false;
                const auto finishAbandonedTileLoad = [&self,
                                                       &completedTileLoad]() {
                    if (completedTileLoad ||
                        self->state->alive.load(std::memory_order_acquire)) {
                        return;
                    }
                    self->releaseThrottleSlotOnce();
                    completedTileLoad = true;
                    self->state->resolveDestructionIfComplete();
                };
                const auto finishCompose = [&self]() {
                    self->state->activeRasterComposeTasks.fetch_sub(
                        1,
                        std::memory_order_relaxed);
                    self->state->resolveDestructionIfComplete();
                    // [2026-08-21 冻屏根修] compose 落地:同步 Landing 票。
                    RasterOverlayTileProvider::
                        syncRasterLandingTicketFromAnyThread(self->state);
                };

                // Provider teardown drains frame-pending tasks inline. Avoid
                // doing the expensive composition after the owner is gone;
                // only release lifecycle/throttle state.
                if (!self->state->alive.load(std::memory_order_acquire)) {
                    finishAbandonedTileLoad();
                    finishCompose();
                    return;
                }

                try {
                    CompositeImageResult composed =
                        composeMappedSourceImageSet(
                            *self->scheme,
                            self->targetBounds,
                            std::move(completedSources),
                            self->returnEmptyForAncestorOnly);
                    if (composed.image) {
                        if (self->state->alive.load(
                                std::memory_order_acquire)) {
                            completedTileLoad = true;
                            self->onSuccess(
                                std::move(composed.image),
                                nullptr,
                                projectGeographicToProvider(
                                    composed.rectangle,
                                    self->projection),
                                composed.moreDetailAvailable,
                                std::move(composed.diagnostics),
                                std::move(composed.credits));
                        }
                    } else if (self->state->alive.load(
                                   std::memory_order_acquire)) {
                        completedTileLoad = true;
                        self->onFailure(std::move(composed.diagnostics));
                    }
                    finishAbandonedTileLoad();
                    finishCompose();
                } catch (...) {
                    if (self->state->alive.load(
                            std::memory_order_acquire)) {
                        completedTileLoad = true;
                        self->onFailure({});
                    }
                    finishAbandonedTileLoad();
                    finishCompose();
                }
            };
        if (!state->sceneResourceManaged.load(std::memory_order_acquire)) {
            // Preserve the standalone Provider API contract: without a Scene
            // budget, composition is dispatched immediately as it was before
            // Scene-level worker admission was introduced.
            try {
                (void)AsyncSystem::pool().enqueue(composeTask);
            } catch (...) {
                composeTask();
            }
            return;
        }

        try {
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->pendingRasterComposeTasks.push_back(
                    std::move(composeTask));
            }
            RasterOverlayTileProvider::syncRasterLandingTicketFromAnyThread(
                state);
        } catch (...) {
            state->activeRasterComposeTasks.fetch_sub(
                1,
                std::memory_order_relaxed);
            state->resolveDestructionIfComplete();
            RasterOverlayTileProvider::syncRasterLandingTicketFromAnyThread(
                state);
            onFailure({});
        }
    }

    std::unique_ptr<TileScheme> scheme;
    std::shared_ptr<ProviderAsyncState> state;
    std::shared_ptr<std::atomic<bool>> slotReleased;
    std::shared_ptr<QuadtreeSourceAssetDepot> depot;
    uint64_t waiterOwnerToken = 0;
    RasterSourceTileMapping sourceTiles;
    Rectangle targetBounds;
    RasterOverlayProjection projection;
    int maximumLevel = 0;
    bool returnEmptyForAncestorOnly = false;
    bool directTerminalFailure = false;
    MappedSourceLoadSuccess onSuccess;
    MappedSourceLoadFailure onFailure;
    mutable std::mutex mutex;
    int remaining = 0;
    std::vector<bool> sourceIssued;
    bool completed = false;
    std::vector<RasterSourceResult> sources;
};

}  // namespace earth_engine
