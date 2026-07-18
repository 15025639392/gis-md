#pragma once

#include "TileContentUploadCommitter.h"
#include "TileEmptyContentRegistry.h"
#include "TileLoadDomainPolicy.h"
#include "TileLoadLifecycle.h"
#include "TileLoadTypes.h"
#include "TilePendingUploadCompletion.h"
#include "TileTerminalLoadCommitter.h"
#include "TilesetTile.h"
#include "../content/GltfContentProvider.h"
#include "../debug/PerfTimer.h"

#include <cstdio>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace earth_engine {

class ActivatedRasterOverlay;
class IPrepareRendererResources;
class RenderDevice;

class TilePendingLoadCommitCoordinator {
public:
    static void captureInitialBoundingVolumes(
        TilesetTile& tile,
        TileLoadResultMetadata& metadata) {
        if (!metadata.initialBoundingVolume && tile.boundingVolume) {
            metadata.initialBoundingVolume = *tile.boundingVolume;
        }
        if (!metadata.initialContentBoundingVolume &&
            tile.contentBoundingVolume) {
            metadata.initialContentBoundingVolume =
                *tile.contentBoundingVolume;
        }
    }

    template <typename EnsureTileFn,
              typename EnsureChildrenFn,
              typename MarkResourcesDirtyFn>
    static void commitTerminalResult(
        PendingTileLoad& result,
        TileEmptyContentRegistry& emptyContentRegistry,
        IPrepareRendererResources* pPrepRenderer,
        EnsureTileFn&& ensureTile,
        EnsureChildrenFn&& ensureChildren,
        MarkResourcesDirtyFn&& markResourcesDirty,
        TilesetContentProvider* contentProvider = nullptr) {
        TilesetTile* tile = ensureTile(result.key);
        if (!tile) {
            emptyContentRegistry.erase(result.cacheKey);
            return;
        }

        const TileTerminalLoadAction action =
            TileTerminalLoadCommitter::commitTerminalResult(
                result.domain,
                *tile,
                result.cacheKey,
                std::move(result.result),
                emptyContentRegistry,
                pPrepRenderer,
                contentProvider);
        applyCommitAction(action, *tile, ensureChildren, markResourcesDirty);
    }

    template <typename EnsureTileFn,
              typename EnsureChildrenFn,
              typename EnsureGltfResourcesFn,
              typename MarkResourcesDirtyFn>
    static void commitUpload(
        PendingTileLoad& upload,
        TilesetContentProvider* contentProvider,
        RenderDevice* device,
        IPrepareRendererResources* pPrepRenderer,
        const std::vector<ActivatedRasterOverlay*>& rasterOverlays,
        TileEmptyContentRegistry& emptyContentRegistry,
        TileLoadLifecycle& lifecycle,
        EnsureTileFn&& ensureTile,
        EnsureChildrenFn&& ensureChildren,
        EnsureGltfResourcesFn&& ensureGltfResources,
        MarkResourcesDirtyFn&& markResourcesDirty,
        uint64_t diagnosticFrameNumber = 0) {
        const double ensureTileStartMs = perf::nowMs();
        TilesetTile* tile = ensureTile(upload.key);
        logSlowCommitPhase(
            diagnosticFrameNumber,
            "TilePendingLoad.commitUpload.ensureTile",
            perf::nowMs() - ensureTileStartMs,
            upload);
        if (!tile) {
            emptyContentRegistry.erase(upload.cacheKey);
            TilePendingUploadCompletion::eraseUpload(
                lifecycle,
                upload.cacheKey);
            return;
        }

        if (TileLoadDomainPolicy::shouldFailUploadForDomain(
                upload.domain,
                upload.result)) {
            const double failCommitStartMs = perf::nowMs();
            TileLoadResult failedResult =
                TileLoadDomainPolicy::normalizeForDomain(
                    upload.domain,
                    std::move(upload.result));
            const TileTerminalLoadAction action =
                TileTerminalLoadCommitter::commitTerminalResult(
                    upload.domain,
                    *tile,
                    upload.cacheKey,
                    std::move(failedResult),
                    emptyContentRegistry,
                    pPrepRenderer,
                    contentProvider);
            applyCommitAction(action, *tile, ensureChildren, markResourcesDirty);
            TilePendingUploadCompletion::eraseUpload(
                lifecycle,
                upload.cacheKey);
            logSlowCommitPhase(
                diagnosticFrameNumber,
                "TilePendingLoad.commitUpload.failTerminal",
                perf::nowMs() - failCommitStartMs,
                upload);
            return;
        }

        const double metadataStartMs = perf::nowMs();
        captureInitialBoundingVolumes(*tile, upload.content().metadata);
        logSlowCommitPhase(
            diagnosticFrameNumber,
            "TilePendingLoad.commitUpload.metadata",
            perf::nowMs() - metadataStartMs,
            upload);

        const double prepareRenderStartMs = perf::nowMs();
        TileContentUploadCommitter::prepareRenderContent(
            *tile,
            std::move(upload.content()),
            rasterOverlays,
            device,
            pPrepRenderer);
        logSlowCommitPhase(
            diagnosticFrameNumber,
            "TilePendingLoad.commitUpload.prepareRenderContent",
            perf::nowMs() - prepareRenderStartMs,
            upload);

        const double ensureResourcesStartMs = perf::nowMs();
        ensureGltfResources(*tile);
        logSlowCommitPhase(
            diagnosticFrameNumber,
            "TilePendingLoad.commitUpload.ensureGltfResources",
            perf::nowMs() - ensureResourcesStartMs,
            upload);

        // Async pipeline: CPU work dispatched to worker thread; GPU upload
        // and finalization happen in drainGpuUploadQueue.  Keep the tile
        // alive by NOT erasing the upload from the lifecycle — the upload
        // stays as a pending claim that protects the tile from unloading.
        if (tile->content.renderContent.asyncGpuUploadPending) {
            markResourcesDirty(*tile);
            emptyContentRegistry.erase(upload.cacheKey);
            return;
        }

        const bool renderResourcesReady =
            tile->content.renderContent.isRenderContentReady();
        const double finishStartMs = perf::nowMs();
        const TileContentUploadCommitAction action =
            TileContentUploadCommitter::finishRenderResourcePreparation(
                *tile,
                renderResourcesReady,
                pPrepRenderer);
        applyCommitAction(action, *tile, ensureChildren, markResourcesDirty);
        logSlowCommitPhase(
            diagnosticFrameNumber,
            "TilePendingLoad.commitUpload.finish",
            perf::nowMs() - finishStartMs,
            upload);

        emptyContentRegistry.erase(upload.cacheKey);
        TilePendingUploadCompletion::eraseUpload(
            lifecycle,
            upload.cacheKey);
    }

private:
    template <typename CommitAction,
              typename EnsureChildrenFn,
              typename MarkResourcesDirtyFn>
    static void applyCommitAction(
        const CommitAction& action,
        TilesetTile& tile,
        EnsureChildrenFn&& ensureChildren,
        MarkResourcesDirtyFn&& markResourcesDirty) {
        if (action.ensureChildren) {
            ensureChildren(tile);
        }
        if (action.resourcesDirty) {
            markResourcesDirty(tile);
        }
    }

    static void logSlowCommitPhase(uint64_t frameNumber,
                                   const char* scope,
                                   double elapsedMs,
                                   const PendingTileLoad& load) {
        constexpr double kSlowCommitPhaseMs = 1.0;
        if (elapsedMs < kSlowCommitPhaseMs) {
            return;
        }
        char detail[256];
        std::snprintf(
            detail,
            sizeof(detail),
            "domain=%d group=%d priority=%.3f cache=%s",
            static_cast<int>(load.domain),
            static_cast<int>(load.group),
            load.priority,
            load.cacheKey.c_str());
        platformLog(
            LogLevel::Info,
            "EarthPerf",
            "frame=%llu scope=%s ms=%.3f %s",
            static_cast<unsigned long long>(frameNumber),
            scope,
            elapsedMs,
            detail);
    }

};

} // namespace earth_engine
