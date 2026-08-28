// I-P4 第四刀 4b:load/issue 簇(从 RasterOverlayTileProvider.cpp 逐字拆出)
// 内容=loadMappedRasterTile/pumpLoadingMappedRasterTile/loadSourceTileList/
// loadSourceImageSet/issueMappedSourceImageSet/estimateNewSourceRequests/
// mappedTileWouldIssueNewSourceRequests/issuePendingSourceFallbacks/
// issueActiveMappedSourceImageSets,以及 5 个专属匿名 helper。
// 行为逐字等价是硬约束:与拆出前逐字符一致,不夹带任何改动。

#include "RasterOverlayTileProvider.h"
#include "RasterOverlayImageCompositing.h"
#include "RasterOverlaySourceDepot.h"
#include "ImageryProvider.h"
#include "../core/resources/FrameResourceBudget.h"
#include "../threading/CancellationToken.h"
#include "../core/async/AsyncSystem.h"
#include "../debug/PerfTimer.h"
#include "../debug/PlatformLog.h"

#include <algorithm>
#include <atomic>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace earth_engine {
namespace {

void logAndroidRasterPipeline(const char* stage,
                              const std::string& cacheKey,
                              int sourceCount,
                              int sourceZoom) {
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1, std::memory_order_relaxed) >= 48) {
        return;
    }
    platformLog(
        LogLevel::Info,
        "RasterOverlayPipe",
        "%s cache=%s sources=%d sourceZoom=%d",
        stage,
        cacheKey.c_str(),
        sourceCount,
        sourceZoom);
}


int availableRasterRequestSlots(FrameResourceBudget* budget,
                                uint32_t currentInflight) {
    if (!budget) {
        return std::numeric_limits<int>::max();
    }
    const FrameResourceBudgetSnapshot snapshot = budget->snapshot();
    if (currentInflight >= snapshot.maxRasterNetworkInflight ||
        snapshot.rasterNetworkRequestsIssued >=
            snapshot.maxRasterNetworkRequestsPerFrame) {
        return 0;
    }
    const uint32_t frameSlots =
        snapshot.maxRasterNetworkRequestsPerFrame -
        snapshot.rasterNetworkRequestsIssued;
    const uint32_t inflightSlots =
        snapshot.maxRasterNetworkInflight - currentInflight;
    // Scene availability is consumed atomically in requestSource immediately
    // before provider.requestTile. Do not fold it into this planning probe:
    // doing so would leave denied work outside the provider's retryable source
    // set and would not record a real admission denial for next-frame demand.
    return static_cast<int>(std::min(frameSlots, inflightSlots));
}

bool hasRasterInflightCapacity(FrameResourceBudget* budget,
                               uint32_t currentInflight,
                               int estimatedFanout) {
    if (!budget) {
        return true;
    }
    return budget->hasNetworkInflightCapacity(
        FrameResourceLane::RasterRequest,
        currentInflight,
        estimatedFanout);
}

bool isTransientRasterSourceFailure(
    const std::vector<std::string>& diagnostics) {
    return std::any_of(
        diagnostics.begin(),
        diagnostics.end(),
        [](const std::string& diagnostic) {
            return diagnostic.find(
                       "Raster source tile request threw before completion") !=
                   std::string::npos;
        });
}

} // namespace

bool RasterOverlayTileProvider::loadMappedRasterTile(
    RasterOverlayTile& tile,
    FrameResourceBudget* budget) {
    if (budget && budget->sceneArbiter() != nullptr) {
        asyncState_->sceneResourceManaged.store(
            true,
            std::memory_order_release);
    }
    auto loadState = tile.getState();
    switch (loadState) {
        case RasterOverlayTile::LoadState::Unloaded:
            break;
        case RasterOverlayTile::LoadState::Loading:
            pumpLoadingMappedRasterTile(tile, budget);
            return true;
        case RasterOverlayTile::LoadState::Loaded:
        case RasterOverlayTile::LoadState::Done:
        case RasterOverlayTile::LoadState::Failed:
        case RasterOverlayTile::LoadState::Placeholder:
            return true;
    }

    const std::string ck = tile.getCacheKey();
    if (ck.empty()) return false;

    const Rectangle outputBounds = tile.getRectangle();
    const Rectangle targetBounds =
        unprojectProviderToGeographic(outputBounds, projection_);
    RasterSourceTileMapping sourceTiles;
    const Rectangle effectiveCoverage =
        effectiveCoverageRectangle(scheme_, sourceCoverageRectangle_);
    if (tile.hasMappedSourceList() &&
        tile.getMappedSourceBounds().computeIntersection(
            effectiveCoverage)) {
        sourceTiles.sourceZoom = tile.getMappedSourceZoom();
        sourceTiles.sourceBounds = tile.getMappedSourceBounds();
        sourceTiles.sourceKeys = tile.getMappedSourceKeys();
        sourceTiles.minX = tile.getMappedSourceMinX();
        sourceTiles.minY = tile.getMappedSourceMinY();
        sourceTiles.maxX = tile.getMappedSourceMaxX();
        sourceTiles.maxY = tile.getMappedSourceMaxY();
    } else {
        const std::optional<Rectangle> mappedSourceBounds =
            mapGeometryBoundsToImageryCoverage(
                targetBounds,
                effectiveCoverage,
                shouldClampOutsideCoverage(owner_));
        if (!mappedSourceBounds) {
            logAndroidRasterPipeline("coverage-miss", ck, 0, 0);
            tile.setMoreDetailAvailable(
                RasterOverlayTile::MoreDetailAvailable::No);
            tile.setState(RasterOverlayTile::LoadState::Failed);
            return false;
        }
        QuadtreeSourcePlan sourcePlan = buildQuadtreeSourcePlan(
            scheme_,
            provider_,
            textureUploader_.get(),
            targetBounds,
            *mappedSourceBounds,
            tile.getTargetScreenPixelsX(),
            tile.getTargetScreenPixelsY(),
            maximumScreenSpaceError_,
            maximumTextureSize_,
            getMinimumLevel(),
            getMaximumLevel());
        tile.setMappedSourceList(
            sourcePlan.sourceZoom,
            *mappedSourceBounds,
            sourcePlan.sourceKeys,
            sourcePlan.minX,
            sourcePlan.minY,
            sourcePlan.maxX,
            sourcePlan.maxY);
        sourceTiles = RasterSourceTileMapping{
            sourcePlan.sourceZoom,
            *mappedSourceBounds,
            sourcePlan.sourceKeys,
            sourcePlan.minX,
            sourcePlan.minY,
            sourcePlan.maxX,
            sourcePlan.maxY};
    }

    if (sourceTiles.empty()) {
        logAndroidRasterPipeline("empty-plan", ck, 0, sourceTiles.sourceZoom);
        tile.setMoreDetailAvailable(RasterOverlayTile::MoreDetailAvailable::No);
        tile.setState(RasterOverlayTile::LoadState::Failed);
        return false;
    }
    logAndroidRasterPipeline(
        "start",
        ck,
        static_cast<int>(sourceTiles.sourceKeys.size()),
        sourceTiles.sourceZoom);
    return loadSourceTileList(
        tile,
        std::move(sourceTiles),
        targetBounds,
        ck,
        budget);
}

bool RasterOverlayTileProvider::pumpLoadingMappedRasterTile(
    RasterOverlayTile& tile,
    FrameResourceBudget* budget) {
    if (!tile.isMappedRasterTile() ||
        tile.getState() != RasterOverlayTile::LoadState::Loading) {
        return false;
    }

    std::shared_ptr<MappedSourceImageSet> sourceSet;
    {
        std::unique_lock<std::mutex> lock(asyncState_->mutex);
        auto it = asyncState_->activeMappedSourceSets.find(
            tile.getCacheKey());
        if (it != asyncState_->activeMappedSourceSets.end()) {
            sourceSet = it->second;
        }
    }
    if (sourceSet && sourceSet->hasUnissuedSources()) {
        issueMappedSourceImageSet(sourceSet, budget);
    }
    return true;
}

bool RasterOverlayTileProvider::loadSourceTileList(
    RasterOverlayTile& tile,
    RasterSourceTileMapping sourceTiles,
    const Rectangle& targetBounds,
    const std::string& cacheKey,
    FrameResourceBudget* budget) {
    const Rectangle composeBounds =
        sourceTiles.sourceBounds.isEmpty() ? targetBounds
                                           : sourceTiles.sourceBounds;
    return loadSourceImageSet(
        tile,
        std::move(sourceTiles),
        composeBounds,
        cacheKey,
        budget);
}

bool RasterOverlayTileProvider::loadSourceImageSet(
    RasterOverlayTile& tile,
    RasterSourceTileMapping sourceTiles,
    const Rectangle& targetBounds,
    const std::string& cacheKey,
    FrameResourceBudget* budget) {
    if (cacheKey.empty()) return false;
    {
        std::lock_guard<std::mutex> lock(asyncState_->mutex);
        if (asyncState_->inFlightRequests.count(cacheKey)) return true;
    }

    const int estimatedNewSourceRequests =
        estimateNewSourceRequestsForSourceKeys(sourceTiles.sourceKeys);
    const int availableSourceRequestSlots = availableRasterRequestSlots(
        budget,
        asyncState_->activeRasterSourceRequests.load(
            std::memory_order_relaxed));
    // cesium-native SharedAssetDepot::getOrCreate returns an existing pending
    // or loaded asset without starting transport work. Preserve that budget
    // behavior for both mapped and direct raster loads.
    const bool hasReusableSharedSource =
        estimatedNewSourceRequests <
            static_cast<int>(sourceTiles.sourceKeys.size());
    if (estimatedNewSourceRequests > 0 &&
        availableSourceRequestSlots <= 0 &&
        !hasReusableSharedSource) {
        return false;
    }
    if (!tile.isMappedRasterTile() && estimatedNewSourceRequests > 0 &&
        !hasRasterInflightCapacity(
            budget,
            asyncState_->activeRasterSourceRequests.load(
                std::memory_order_relaxed),
            estimatedNewSourceRequests)) {
        return false;
    }

    tile.setState(RasterOverlayTile::LoadState::Loading);
    asyncState_->activeRasterTileLoads.fetch_add(
        1,
        std::memory_order_relaxed);
    // 本次加载名额的唯一释放令牌：完成回调与 abandon/析构共用，谁先
    // exchange 谁递减（见 releaseRasterThrottleSlotOnce）
    auto throttleSlotReleased = std::make_shared<std::atomic<bool>>(false);
    {
        std::lock_guard<std::mutex> lock(asyncState_->mutex);
        asyncState_->inFlightRequests.insert(cacheKey);
    }

    std::shared_ptr<ProviderAsyncState> state = asyncState_;
    std::weak_ptr<RasterOverlayTile> tileWeak;
    auto tileIt = tiles_.find(cacheKey);
    if (tileIt != tiles_.end()) {
        tileWeak = tileIt->second;
    }
    if (!sourceAssetDepot_) {
        refreshSourceAssetDepot();
    }
    uint64_t requestSourceDepotEpoch = 0;
    {
        std::lock_guard<std::mutex> lock(asyncState_->mutex);
        requestSourceDepotEpoch = asyncState_->sourceTileDepotEpoch;
    }
    const uint64_t sourceWaiterOwnerToken = nextSourceWaiterOwnerToken();
    const bool returnEmptyForAncestorOnly = true;
    auto sourceSet = std::make_shared<MappedSourceImageSet>(
        scheme_,
        state,
        throttleSlotReleased,
        sourceAssetDepot_,
        sourceWaiterOwnerToken,
        std::move(sourceTiles),
        targetBounds,
        projection_,
        getMaximumLevel(),
        returnEmptyForAncestorOnly,
        !tile.isMappedRasterTile(),
        [state, throttleSlotReleased, cacheKey, tileWeak,
         requestSourceDepotEpoch, sourceWaiterOwnerToken](
            std::unique_ptr<DecodedImage> composed,
            std::shared_ptr<const DecodedImage> sharedImage,
            Rectangle rectangle,
            RasterOverlayTile::MoreDetailAvailable moreDetailAvailable,
            std::vector<std::string> diagnostics,
            std::vector<std::string> credits) {
            RetiredAsyncResources retired;
            std::unique_lock<std::mutex> providerLock(state->mutex);
            state->inFlightRequests.erase(cacheKey);
            auto sourceSetIt =
                state->activeMappedSourceSets.find(cacheKey);
            if (sourceSetIt != state->activeMappedSourceSets.end()) {
                retired.sourceSets.push_back(
                    std::move(sourceSetIt->second));
                state->activeMappedSourceSets.erase(sourceSetIt);
            }
            state->sourceTileDepotFallbackKeysByOwner.erase(
                sourceWaiterOwnerToken);
            state->activeMappedSourceOwnerTokens.erase(
                sourceWaiterOwnerToken);
            compactActiveMappedSourceSetOrderLocked(*state);
            if (!state->alive.load(std::memory_order_acquire)) {
                releaseRasterThrottleSlotOnce(
                    *throttleSlotReleased,
                    state->activeRasterTileLoads);
                state->resolveDestructionIfComplete();
                return;
            }
            if (state->sourceTileDepotEpoch != requestSourceDepotEpoch) {
                if (auto tile = tileWeak.lock()) {
                    tile->setMoreDetailAvailable(
                        RasterOverlayTile::MoreDetailAvailable::No);
                    tile->setState(RasterOverlayTile::LoadState::Failed);
                }
                releaseRasterThrottleSlotOnce(
                    *throttleSlotReleased,
                    state->activeRasterTileLoads);
                state->resolveDestructionIfComplete();
                state->revision.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            // 祖先-only 空合成走的也是这条成功路径,此前与正常瓦同名打
            // "composed",在管线日志里无法区分 —— 空洞排查时分不清"合成出空图"
            // 与"合成出内容"。按图尺寸分名,不新增任何机制。
            const bool emptyComposition =
                composed && composed->width == 0 && composed->height == 0;
            logAndroidRasterPipeline(
                emptyComposition ? "composed-empty" : "composed",
                cacheKey,
                0,
                0);
            PendingUpload pendingUpload{
                cacheKey,
                std::move(composed),
                std::move(sharedImage),
                rectangle,
                moreDetailAvailable,
                std::move(diagnostics),
                std::move(credits)};
            retainPendingUploadImageBytesLocked(*state, pendingUpload);
            enforceSourceDepotBudgetLocked(*state, retired);
            state->pendingUploads.push_back(
                std::move(pendingUpload));
            // cesium _totalTilesCurrentlyLoading 语义：节流名额在加载
            // （下载+合成）完成时释放；GPU 上传属主线程 prepare 阶段，
            // 由 RasterTextureUpload lane 单独限速。此前名额持有到上传
            // 消费，交互期上传被 defer 时节流被积压占满（真机 54/20），
            // 新加载全部被卡。
            releaseRasterThrottleSlotOnce(
                *throttleSlotReleased,
                state->activeRasterTileLoads);
            state->resolveDestructionIfComplete();
            // [2026-08-21 冻屏根修] 本件在途落地:Landing 票按剩余在途同步,
            // 最后一件时释放 → 触发落地唤醒(睡着的循环被踹醒去消费上传)。
            RasterOverlayTileProvider::syncRasterLandingTicketLocked(
                state);
        },
        [state, throttleSlotReleased, cacheKey, tileWeak,
             requestSourceDepotEpoch, sourceWaiterOwnerToken](
            std::vector<std::string> diagnostics) {
            RetiredAsyncResources retired;
            std::unique_lock<std::mutex> providerLock(state->mutex);
            state->inFlightRequests.erase(cacheKey);
            auto sourceSetIt =
                state->activeMappedSourceSets.find(cacheKey);
            if (sourceSetIt != state->activeMappedSourceSets.end()) {
                retired.sourceSets.push_back(
                    std::move(sourceSetIt->second));
                state->activeMappedSourceSets.erase(sourceSetIt);
            }
            state->sourceTileDepotFallbackKeysByOwner.erase(
                sourceWaiterOwnerToken);
            state->activeMappedSourceOwnerTokens.erase(
                sourceWaiterOwnerToken);
            compactActiveMappedSourceSetOrderLocked(*state);
            if (!state->alive.load(std::memory_order_acquire)) {
                releaseRasterThrottleSlotOnce(
                    *throttleSlotReleased,
                    state->activeRasterTileLoads);
                state->resolveDestructionIfComplete();
                return;
            }
            if (state->sourceTileDepotEpoch != requestSourceDepotEpoch) {
                if (auto tile = tileWeak.lock()) {
                    tile->setMoreDetailAvailable(
                        RasterOverlayTile::MoreDetailAvailable::No);
                    tile->setState(RasterOverlayTile::LoadState::Failed);
                }
                releaseRasterThrottleSlotOnce(
                    *throttleSlotReleased,
                    state->activeRasterTileLoads);
                state->resolveDestructionIfComplete();
                state->revision.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            logAndroidRasterPipeline("compose-failed", cacheKey, 0, 0);
            if (auto tile = tileWeak.lock()) {
                const bool transientFailure =
                    isTransientRasterSourceFailure(diagnostics);
                tile->setLoadDiagnostics(std::move(diagnostics));
                tile->setMoreDetailAvailable(
                    RasterOverlayTile::MoreDetailAvailable::No);
                tile->setState(
                    transientFailure
                        ? RasterOverlayTile::LoadState::Unloaded
                        : RasterOverlayTile::LoadState::Failed);
            }
            releaseRasterThrottleSlotOnce(
                *throttleSlotReleased,
                state->activeRasterTileLoads);
            state->resolveDestructionIfComplete();
            state->revision.fetch_add(1, std::memory_order_relaxed);
            RasterOverlayTileProvider::syncRasterLandingTicketLocked(
                state);
        });

    {
        std::lock_guard<std::mutex> lock(asyncState_->mutex);
        auto [it, inserted] =
            asyncState_->activeMappedSourceSets.emplace(cacheKey, sourceSet);
        if (!inserted && it->second) {
            asyncState_->activeMappedSourceOwnerTokens.erase(
                it->second->getWaiterOwnerToken());
        }
        asyncState_->activeMappedSourceOwnerTokens.insert(
            sourceWaiterOwnerToken);
        if (!inserted) {
            it->second = sourceSet;
        } else {
            asyncState_->activeMappedSourceSetOrder.push_back(cacheKey);
        }
    }

    issueMappedSourceImageSet(sourceSet, budget);

    return true;
}

int RasterOverlayTileProvider::issueMappedSourceImageSet(
    const std::shared_ptr<MappedSourceImageSet>& sourceSet,
    FrameResourceBudget* budget) {
    if (pendingUploadBackpressureActive()) {
        return 0;
    }
    if (!sourceSet || sourceSet->isComplete()) {
        return 0;
    }
    std::shared_ptr<ProviderAsyncState> state = asyncState_;
    auto onSourceIssued = [state]() {
        state->rasterSourceRequestsStarted.fetch_add(
            1,
            std::memory_order_relaxed);
        const uint32_t active =
            state->activeRasterSourceRequests.fetch_add(
                1,
                std::memory_order_relaxed) +
            1;
        uint32_t peak = state->peakRasterSourceRequests.load(
            std::memory_order_relaxed);
        while (active > peak &&
               !state->peakRasterSourceRequests.compare_exchange_weak(
                   peak,
                   active,
                   std::memory_order_relaxed,
                   std::memory_order_relaxed)) {
        }
        // [2026-08-21 冻屏根修] worker 派发源请求:确保持有 Landing 票。
        RasterOverlayTileProvider::syncRasterLandingTicketFromAnyThread(state);
    };
    auto onSourceFinished = [state]() {
        state->rasterSourceRequestsCompleted.fetch_add(
            1,
            std::memory_order_relaxed);
        uint32_t current = state->activeRasterSourceRequests.load(
            std::memory_order_relaxed);
        while (current > 0 &&
               !state->activeRasterSourceRequests.compare_exchange_weak(
                   current,
                   current - 1,
                   std::memory_order_relaxed,
                   std::memory_order_relaxed)) {
        }
        state->resolveDestructionIfComplete();
        // [2026-08-21 冻屏根修] 源请求落地:同步 Landing 票。
        RasterOverlayTileProvider::syncRasterLandingTicketFromAnyThread(state);
    };
    auto onSourceFailed = [state]() {
        state->rasterSourceRequestsFailed.fetch_add(
            1,
            std::memory_order_relaxed);
    };

    const int maxToIssue = availableRasterRequestSlots(
        budget,
        state->activeRasterSourceRequests.load(
            std::memory_order_relaxed));
    std::function<bool()> tryAdmitSource;
    if (budget) {
        tryAdmitSource = [budget]() {
            return budget->tryIssue(
                FrameResourceLane::RasterRequest,
                FrameResourcePriority::Normal,
                1);
        };
    }
    const int newlyIssued =
        sourceSet->issueSome(
            maxToIssue,
            tryAdmitSource,
            onSourceIssued,
            onSourceFinished,
            onSourceFailed);
    return newlyIssued;
}

int RasterOverlayTileProvider::estimateNewSourceRequestsForSourceKeys(
    const std::vector<TileKey>& sourceKeys) const {
    int estimated = 0;
    std::unique_lock<std::mutex> lock(asyncState_->mutex);
    for (const TileKey& sourceKey : sourceKeys) {
        const std::string sourceKeyString =
            sourceCacheKey(
                asyncState_->sourceTileDepotEpoch,
                sourceKey);
        auto cached =
            asyncState_->sourceTileDepotCache.find(sourceKeyString);
        if (cached != asyncState_->sourceTileDepotCache.end() &&
            (cached->second.image || cached->second.terminalFailure)) {
            continue;
        }
        if (asyncState_->sourceTileDepotInFlight.count(sourceKeyString) > 0) {
            continue;
        }
        ++estimated;
    }
    return estimated;
}

bool RasterOverlayTileProvider::mappedTileWouldIssueNewSourceRequests(
    const RasterOverlayTile& tile) const {
    if (!tile.isMappedRasterTile() || !tile.hasMappedSourceList()) {
        return true;
    }
    return estimateNewSourceRequestsForSourceKeys(
               tile.getMappedSourceKeys()) > 0;
}

int RasterOverlayTileProvider::issuePendingSourceFallbacks(
    FrameResourceBudget* budget) {
    const double totalStartMs = perf::nowMs();
    if (asyncState_->pendingSourceFallbackCount.load(
            std::memory_order_acquire) == 0) {
        return 0;
    }
    const double backpressureStartMs = perf::nowMs();
    const bool backpressureActive = pendingUploadBackpressureActive();
    const double backpressureMs =
        perf::nowMs() - backpressureStartMs;
    if (backpressureActive) {
        if (backpressureMs >= 1.0) {
            platformLog(
                LogLevel::Info,
                "EarthPerf",
                "RasterFallback.pump ms=%.2f backpressure=%.2f select=0.00 issue=0.00 processed=0 issued=0 blocked=1",
                perf::nowMs() - totalStartMs,
                backpressureMs);
        }
        return 0;
    }
    int issued = 0;
    int processed = 0;
    double issueMs = 0.0;
    while (true) {
        PendingSourceFallback fallback;
        bool ownerActive = true;
        bool canReuseExistingSource = false;
        std::optional<TileKey> requestedKey;
        {
            std::lock_guard<std::mutex> lock(asyncState_->mutex);
            if (asyncState_->pendingSourceFallbacks.empty()) {
                break;
            }
            requestedKey = asyncState_->pendingSourceFallbacks.front().requestedKey;
        }
        if (requestedKey && sourceAssetDepot_) {
            canReuseExistingSource =
                !sourceAssetDepot_->wouldIssueNewRequest(*requestedKey);
        }

        if (budget && !canReuseExistingSource) {
            const int remainingSlots = availableRasterRequestSlots(
                budget,
                asyncState_->activeRasterSourceRequests.load(
                    std::memory_order_relaxed));
            if (remainingSlots <= 0) {
                break;
            }
        }

        {
            std::lock_guard<std::mutex> lock(asyncState_->mutex);
            if (asyncState_->pendingSourceFallbacks.empty()) {
                break;
            }
            fallback =
                std::move(asyncState_->pendingSourceFallbacks.front());
            asyncState_->pendingSourceFallbacks.pop_front();
            asyncState_->pendingSourceFallbackCount.store(
                static_cast<uint32_t>(
                    asyncState_->pendingSourceFallbacks.size()),
                std::memory_order_release);
            ownerActive =
                fallback.ownerToken == 0 ||
                asyncState_->activeMappedSourceOwnerTokens.count(
                    fallback.ownerToken) > 0;
        }

        if (!ownerActive) {
            if (sourceAssetDepot_) {
                sourceAssetDepot_->abandonInFlightSource(fallback.originalKey);
            }
            continue;
        }

        ++processed;
        const double issueStartMs = perf::nowMs();
        std::function<bool()> tryAdmitSource;
        if (budget) {
            tryAdmitSource = [budget]() {
                return budget->tryIssue(
                    FrameResourceLane::RasterRequest,
                    FrameResourcePriority::Normal,
                    1);
            };
        }
        const int newlyIssued =
            fallback.issue ? fallback.issue(std::move(tryAdmitSource)) : 0;
        issueMs += perf::nowMs() - issueStartMs;
        if (newlyIssued < 0) {
            std::lock_guard<std::mutex> lock(asyncState_->mutex);
            asyncState_->pendingSourceFallbacks.push_front(
                std::move(fallback));
            asyncState_->pendingSourceFallbackCount.store(
                static_cast<uint32_t>(
                    asyncState_->pendingSourceFallbacks.size()),
                std::memory_order_release);
            break;
        }
        if (newlyIssued > 0) {
            issued += newlyIssued;
        }
    }
    const double totalMs = perf::nowMs() - totalStartMs;
    if (totalMs >= 1.0) {
        platformLog(
            LogLevel::Info,
            "EarthPerf",
            "RasterFallback.pump ms=%.2f backpressure=%.2f select=%.2f issue=%.2f processed=%d issued=%d blocked=0",
            totalMs,
            backpressureMs,
            std::max(0.0, totalMs - backpressureMs - issueMs),
            issueMs,
            processed,
            issued);
    }
    return issued;
}

int RasterOverlayTileProvider::issueActiveMappedSourceImageSets(
    FrameResourceBudget* budget,
    double* fallbackMs,
    double* snapshotMs,
    double* issueMs) {
    const double fallbackStartMs = perf::nowMs();
    int issued = issuePendingSourceFallbacks(budget);
    if (fallbackMs) {
        *fallbackMs += perf::nowMs() - fallbackStartMs;
    }

    const double snapshotStartMs = perf::nowMs();
    std::vector<std::shared_ptr<MappedSourceImageSet>> activeSets;
    {
        std::lock_guard<std::mutex> lock(asyncState_->mutex);
        activeSets.reserve(asyncState_->activeMappedSourceSetOrder.size());
        for (const std::string& cacheKey : asyncState_->activeMappedSourceSetOrder) {
            auto it = asyncState_->activeMappedSourceSets.find(cacheKey);
            if (it == asyncState_->activeMappedSourceSets.end() || !it->second) {
                continue;
            }
            activeSets.push_back(it->second);
        }
        compactActiveMappedSourceSetOrderLocked(*asyncState_);
    }
    if (snapshotMs) {
        *snapshotMs += perf::nowMs() - snapshotStartMs;
    }

    const double issueStartMs = perf::nowMs();
    for (const auto& sourceSet : activeSets) {
        if (!sourceSet || !sourceSet->hasUnissuedSources()) {
            continue;
        }
        issued += issueMappedSourceImageSet(sourceSet, budget);
    }
    if (issueMs) {
        *issueMs += perf::nowMs() - issueStartMs;
    }
    return issued;
}


} // namespace earth_engine
