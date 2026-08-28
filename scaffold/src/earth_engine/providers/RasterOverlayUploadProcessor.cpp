// I-P4 第四刀 5:上传处理簇(从 RasterOverlayTileProvider.cpp 逐字拆出)
// 内容=processPendingUploads/hasPendingWork + 专属常量与 uploadAllowedDuringInteraction。
// 行为逐字等价是硬约束:与拆出前逐字符一致,不夹带任何改动。

#include "RasterOverlayTileProvider.h"
#include "RasterOverlayImageCompositing.h"
#include "RasterTextureUploader.h"
#include "../core/resources/FrameResourceBudget.h"
#include "../core/resources/SceneFrameResourceArbiter.h"
#include "../core/async/AsyncSystem.h"
#include "../debug/PerfTimer.h"
#include "../debug/PlatformLog.h"
#include "../debug/Policies.h"
#include "../renderer/RenderDevice.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace earth_engine {
namespace {

constexpr size_t kDefaultMaximumRasterUploadsPerFrame = 20;
constexpr int kInteractionRasterUploadMaxDimension = 512;
constexpr int64_t kInteractionRasterUploadMaxPixels = 512ll * 512ll;

bool uploadAllowedDuringInteraction(
    const std::string& cacheKey,
    const DecodedImage* image) {
    // 交互期只按单次上传成本（尺寸）过滤；节奏由 budget 的
    // RasterTextureUpload lane 控制（TileFrameResourceBudgetPlanner 在
    // smoothing/交互下给出时间基或 ≤8/帧 的涓流额度）。此前对
    // "direct-composite/" 前缀无条件排除：长交互把影像上传全量积压
    // （真机 60+ pendUp），交互期影像完全停更。≤512² 的 Direct composite
    // 单次上传 <1ms，交由 lane 限额涓流即可。
    (void)cacheKey;
    if (!image) {
        return true;
    }
    if (image->width > kInteractionRasterUploadMaxDimension ||
        image->height > kInteractionRasterUploadMaxDimension) {
        return false;
    }
    const int64_t pixels = static_cast<int64_t>(image->width) *
                           static_cast<int64_t>(image->height);
    return pixels <= kInteractionRasterUploadMaxPixels;
}

} // namespace

TileRasterOverlayUploadResult RasterOverlayTileProvider::processPendingUploads(
    bool interactionActive,
    FrameResourceBudget* budget) {
    syncProviderContentRevision();
    // cesium-native: process completed HTTP responses on main thread.
    // Create GPU textures and mark tiles as Loaded.
    FrameResourceBudget localBudget;
    if (!budget) {
        FrameResourceBudgetConfig config;
        config.maxRasterUploadsPerFrame =
            static_cast<uint32_t>(kDefaultMaximumRasterUploadsPerFrame);
        config.interactionActive = interactionActive;
        config.smoothingActive = interactionActive;
        localBudget.beginFrame(frameNumber_, config);
        budget = &localBudget;
    }

    TileRasterOverlayUploadResult result;
    // Provider callbacks can finish on any thread and outlive the frame that
    // issued their requests. They therefore enqueue compose-ready work into
    // ProviderAsyncState instead of retaining a frame arbiter pointer. The
    // render thread admits that work here against the current frame.
    while (true) {
        {
            std::lock_guard<std::mutex> lock(asyncState_->mutex);
            if (asyncState_->pendingRasterComposeTasks.empty()) {
                break;
            }
        }

        SceneFrameResourceArbiter* sceneArbiter = budget->sceneArbiter();
        if (sceneArbiter != nullptr && sceneArbiter->allocationsSealed() &&
            !sceneArbiter->tryAcquire(
                SceneFrameResourceProducer::Raster,
                SceneFrameResourceStage::WorkerDispatch,
                FrameResourcePriority::Normal)) {
            break;
        }

        std::function<void()> task;
        {
            std::lock_guard<std::mutex> lock(asyncState_->mutex);
            if (asyncState_->pendingRasterComposeTasks.empty()) {
                continue;
            }
            task = std::move(asyncState_->pendingRasterComposeTasks.front());
            asyncState_->pendingRasterComposeTasks.pop_front();
        }
        try {
            // Copy into the pool's packaged task so the local fallback remains
            // callable if enqueue itself rejects during shutdown.
            (void)AsyncSystem::pool().enqueue(task);
        } catch (...) {
            task();
        }
    }

    const double sourcePumpStartMs = perf::nowMs();
    issueActiveDirectCompositeSourceImageSets(
        budget,
        &result.sourceFallbackMs,
        &result.sourceSnapshotMs,
        &result.sourceIssueMs);
    result.selectTaskMs += perf::nowMs() - sourcePumpStartMs;

    struct SelectedPendingUpload {
        PendingUpload upload;
        size_t originalIndex = 0;
    };
    std::deque<SelectedPendingUpload> selectedUploads;
    // 策略生效率的分母:本帧是否存在**符合资格**的积压。交互期被尺寸过滤
    // 推迟的大图是设计意图,不算"有活没做"—— 分母不含它们,冻结(有资格的
    // 活也推进不了)与设计内推迟才分得开。
    bool hadEligiblePendingUpload = false;
    const double batchSelectStartMs = perf::nowMs();
    {
        std::lock_guard<std::mutex> lock(asyncState_->mutex);
        for (const auto& pending : asyncState_->pendingUploads) {
            const DecodedImage* pendingImage = pending.image
                ? pending.image.get()
                : pending.sharedImage.get();
            if (!interactionActive ||
                uploadAllowedDuringInteraction(pending.cacheKey,
                                               pendingImage)) {
                hadEligiblePendingUpload = true;
                break;
            }
        }
        size_t originalIndex = 0;
        for (auto selected = asyncState_->pendingUploads.begin();
             selected != asyncState_->pendingUploads.end() &&
             budget->canFinalize(
                 FrameResourceLane::RasterTextureUpload,
                 FrameResourcePriority::Normal,
                 static_cast<int>(selectedUploads.size() + 1));) {
            const DecodedImage* candidateImage = selected->image
                ? selected->image.get()
                : selected->sharedImage.get();
            if (interactionActive &&
                !uploadAllowedDuringInteraction(
                    selected->cacheKey,
                    candidateImage)) {
                ++selected;
                ++originalIndex;
                continue;
            }
            selectedUploads.push_back(SelectedPendingUpload{
                std::move(*selected),
                originalIndex});
            selected = asyncState_->pendingUploads.erase(selected);
            ++originalIndex;
        }
    }
    const double batchSelectMs = perf::nowMs() - batchSelectStartMs;
    result.selectTaskMs += batchSelectMs;
    result.uploadQueueSelectMs += batchSelectMs;

    std::vector<PendingUpload> completedUploads;
    completedUploads.reserve(selectedUploads.size());
    size_t processedSelectedUploads = 0;
    while (!selectedUploads.empty()) {
        if (!budget->tryFinalize(
                FrameResourceLane::RasterTextureUpload,
                FrameResourcePriority::Normal)) {
            break;
        }
        PendingUpload upload =
            std::move(selectedUploads.front().upload);
        selectedUploads.pop_front();
        ++processedSelectedUploads;
        const double targetSelectStartMs = perf::nowMs();
        std::vector<TilePtr> targetTiles;
        if (auto it = tiles_.find(upload.cacheKey); it != tiles_.end()) {
            targetTiles.push_back(it->second);
        }
        targetTiles.erase(
            std::remove_if(
                targetTiles.begin(),
                targetTiles.end(),
                [this](const TilePtr& target) {
                    return !target ||
                           (target->isDirectCompositeTile() &&
                            !ownsCurrentTile(*target));
                }),
            targetTiles.end());
        const double targetSelectMs =
            perf::nowMs() - targetSelectStartMs;
        result.selectTaskMs += targetSelectMs;
        result.uploadQueueSelectMs += targetSelectMs;
        if (targetTiles.empty()) {
            // 节流名额已在加载完成入队时释放，这里只丢弃孤儿上传
            completedUploads.push_back(std::move(upload));
            continue;
        }

        const DecodedImage* uploadImage = upload.image
            ? upload.image.get()
            : upload.sharedImage.get();
        const bool emptyImage =
            uploadImage &&
            uploadImage->width == 0 &&
            uploadImage->height == 0 &&
            uploadImage->channels == 0 &&
            uploadImage->pixels.empty();

        if (emptyImage) {
            const double finalizeStartMs = perf::nowMs();
            for (const TilePtr& target : targetTiles) {
                target->setLoadDiagnostics(upload.diagnostics);
                target->setCredits(upload.credits);
                target->setMoreDetailAvailable(
                    RasterOverlayTile::MoreDetailAvailable::No);
                target->setRectangle(upload.rectangle);
                target->markLoadedWithoutTexture();
            }
            result.tileFinalizeMs += perf::nowMs() - finalizeStartMs;
            ++result.processedUploads;
            completedUploads.push_back(std::move(upload));
            continue;
        }

        if (!uploadImage || !isDecodedImageUploadable(*uploadImage)) {
            const double finalizeStartMs = perf::nowMs();
            for (const TilePtr& target : targetTiles) {
                target->setLoadDiagnostics(upload.diagnostics);
                target->setCredits(upload.credits);
                target->setMoreDetailAvailable(
                    RasterOverlayTile::MoreDetailAvailable::No);
                target->setState(RasterOverlayTile::LoadState::Failed);
            }
            result.tileFinalizeMs += perf::nowMs() - finalizeStartMs;
            ++result.processedUploads;
            completedUploads.push_back(std::move(upload));
            continue;
        }

        const double uploadStartMs = perf::nowMs();
        double uploadMs = 0.0;
        bool directCompositeUpload = false;
        for (const TilePtr& target : targetTiles) {
            RasterOverlayTile& tile = *target;
            directCompositeUpload =
                directCompositeUpload || tile.isDirectCompositeTile();
            // Resource-prep upload (main-thread safe). Direct composite images
            // are already combined at the selector's target screen-pixel
            // density; on mobile, generating mipmaps for every composite is
            // expensive main-thread work without improving the selected tile.
            const bool generateMipmaps = false;
            RasterTextureUploadOptions uploadOptions;
            uploadOptions.generateMipmaps = generateMipmaps;
            const double uploadTextureStartMs = perf::nowMs();
            auto tex = textureUploader_
                ? textureUploader_->uploadRasterTexture(
                      *uploadImage,
                      uploadOptions)
                : nullptr;
            const double singleUploadMs =
                perf::nowMs() - uploadTextureStartMs;
            result.uploadTextureMs += singleUploadMs;
            uploadMs = perf::nowMs() - uploadStartMs;
            if (singleUploadMs > result.maxUploadMs) {
                result.maxUploadMs = singleUploadMs;
                result.maxUploadWidth = uploadImage->width;
                result.maxUploadHeight = uploadImage->height;
            }
            const double finalizeStartMs = perf::nowMs();
            if (!tex) {
                tile.setLoadDiagnostics(upload.diagnostics);
                tile.setCredits(upload.credits);
                tile.setMoreDetailAvailable(
                    RasterOverlayTile::MoreDetailAvailable::No);
                tile.setState(RasterOverlayTile::LoadState::Failed);
                result.tileFinalizeMs +=
                    perf::nowMs() - finalizeStartMs;
                continue;
            }
            const int sourceLevel =
                tile.isDirectCompositeTile() ? tile.getDirectCompositeSourceZoom() : tile.getTileID().z;
            const RasterOverlayTile::MoreDetailAvailable moreDetailAvailable =
                upload.moreDetailAvailable !=
                        RasterOverlayTile::MoreDetailAvailable::Unknown
                    ? upload.moreDetailAvailable
                    : (sourceLevel < tile.getMaxZoom()
                           ? RasterOverlayTile::MoreDetailAvailable::Yes
                           : RasterOverlayTile::MoreDetailAvailable::No);
            tile.setMoreDetailAvailable(moreDetailAvailable);
            tile.setLoadDiagnostics(upload.diagnostics);
            tile.setCredits(upload.credits);
            tile.setRectangle(upload.rectangle);
            // cesium-native: transfer texture ownership to the tile.
            // The tile owns its texture; no external cache needed.
            tile.setTexture(std::move(tex));
            platformLog(LogLevel::Info, "RasterOverlayTileProvider",
                "Tile loaded: %d/%d/%d", tile.getTileID().z,
                tile.getTileID().x, tile.getTileID().y);
            if (uploadMs >= 8.0 ||
                uploadImage->width > 1024 ||
                uploadImage->height > 1024) {
                platformLog(LogLevel::Info, "RasterOverlayTileProvider",
                    "upload %.2fms size=%dx%d directComposite=%d mipmap=%d cache=%s",
                    uploadMs,
                    uploadImage->width,
                    uploadImage->height,
                    tile.isDirectCompositeTile() ? 1 : 0,
                    generateMipmaps ? 1 : 0,
                    tile.getCacheKey().c_str());
            }
            result.tileFinalizeMs += perf::nowMs() - finalizeStartMs;
        }
        budget->recordElapsed(FrameResourceLane::RasterTextureUpload, uploadMs);
        ++result.processedUploads;
        if (directCompositeUpload) {
            ++result.directCompositeUploads;
        }
        completedUploads.push_back(std::move(upload));
        if (budget->mainThreadTimeExpired()) {
            break;
        }
    }

    if (!selectedUploads.empty()) {
        const double requeueStartMs = perf::nowMs();
        std::lock_guard<std::mutex> lock(asyncState_->mutex);
        while (!selectedUploads.empty()) {
            SelectedPendingUpload selected =
                std::move(selectedUploads.front());
            selectedUploads.pop_front();
            const size_t insertionIndex = std::min(
                selected.originalIndex - processedSelectedUploads,
                asyncState_->pendingUploads.size());
            auto insertionPoint = asyncState_->pendingUploads.begin();
            std::advance(
                insertionPoint,
                static_cast<std::ptrdiff_t>(insertionIndex));
            asyncState_->pendingUploads.insert(
                insertionPoint,
                std::move(selected.upload));
        }
        const double requeueMs = perf::nowMs() - requeueStartMs;
        result.selectTaskMs += requeueMs;
        result.uploadQueueSelectMs += requeueMs;
    }

    // 影像上传推进率:有符合资格积压的帧里,有多少帧真的推进了至少一个上传。
    // 语义同 TilePendingLoadProcessor 的 FinalizeProgress(帧粒度二值);这条
    // lane 正是"交互期硬冻结改 budget 涓流"修复的先行现场,守卫防它复发。
    if (hadEligiblePendingUpload) {
        policy::observe(policy::Id::RasterUploadProgress,
                        processedSelectedUploads > 0 ? 1 : 0, 1);
    }

    if (!completedUploads.empty()) {
        const double bookkeepingStartMs = perf::nowMs();
        if (result.processedUploads > 0) {
            asyncState_->revision.fetch_add(
                static_cast<uint64_t>(result.processedUploads),
                std::memory_order_relaxed);
        }
        std::shared_ptr<ProviderAsyncState> deferredState = asyncState_;
        auto deferredRelease =
            std::make_shared<std::vector<PendingUpload>>(
                std::move(completedUploads));
        deferredState->activeDeferredUploadReleases.fetch_add(
            1,
            std::memory_order_acq_rel);
        auto releaseDeferredUploads =
            [deferredState, deferredRelease]() mutable {
            RetiredAsyncResources retired;
            int64_t destroyedOwnedImageBytes = 0;
            for (PendingUpload& upload : *deferredRelease) {
                if (!upload.image) {
                    continue;
                }
                destroyedOwnedImageBytes +=
                    decodedImageSizeBytes(*upload.image);
                upload.image.reset();
            }
            {
                std::lock_guard<std::mutex> lock(deferredState->mutex);
                releaseOwnedPendingUploadImageBytesLocked(
                    *deferredState,
                    destroyedOwnedImageBytes);
                for (const PendingUpload& upload : *deferredRelease) {
                    if (!upload.sharedImage) {
                        continue;
                    }
                    releasePendingUploadImageBytesLocked(
                        *deferredState,
                        upload);
                }
                enforceSourceDepotBudgetLocked(
                    *deferredState,
                    retired);
            }
            deferredRelease->clear();
            deferredState->activeDeferredUploadReleases.fetch_sub(
                1,
                std::memory_order_acq_rel);
            deferredState->resolveDestructionIfComplete();
            // [2026-08-21 冻屏根修] 延迟释放落地:同步 Landing 票。
            RasterOverlayTileProvider::syncRasterLandingTicketFromAnyThread(
                deferredState);
        };
        try {
            (void)AsyncSystem::pool().enqueue(
                releaseDeferredUploads);
        } catch (...) {
            releaseDeferredUploads();
        }
        result.bookkeepingMs +=
            perf::nowMs() - bookkeepingStartMs;
    }
    result.resourcesDirty = result.processedUploads > 0;
    return result;
}

bool RasterOverlayTileProvider::hasPendingWork() const {
    std::lock_guard<std::mutex> lock(asyncState_->mutex);
    return !asyncState_->pendingUploads.empty() ||
           !asyncState_->inFlightRequests.empty() ||
           !asyncState_->activeDirectCompositeSourceSets.empty() ||
           !asyncState_->pendingSourceFallbacks.empty() ||
           !asyncState_->sourceTileDepotInFlight.empty() ||
           asyncState_->activeRasterComposeTasks.load(
               std::memory_order_relaxed) > 0 ||
           asyncState_->activeDeferredUploadReleases.load(
               std::memory_order_relaxed) > 0 ||
           asyncState_->activeRasterSourceRequests.load(
               std::memory_order_relaxed) > 0;
}

} // namespace earth_engine
