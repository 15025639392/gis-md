#pragma once

#include "TilesetContentLifecycleCoordinator.h"
#include "TileRasterOverlayFrameProcessor.h"
#include "../core/resources/FrameResourceBudget.h"
#include "../layers/ActivatedRasterOverlay.h"
#include "../debug/PerfTimer.h"

#include <vector>

namespace earth_engine {

struct TileUpdateUploadRunInput {
    std::vector<ActivatedRasterOverlay*>& rasterOverlays;
    FrameResourceBudget& frameResourceBudget;
    bool interactionActive = false;
    bool resourceSmoothingActive = false;
};

struct TileUpdateUploadRunResult {
    double terrainUploadMs = 0.0;
    double rasterUploadMs = 0.0;
    int rasterUploadsProcessed = 0;
    int rasterMappedUploadsProcessed = 0;
    double rasterUploadMaxMs = 0.0;
    int rasterUploadMaxWidth = 0;
    int rasterUploadMaxHeight = 0;
    double rasterSelectTaskMs = 0.0;
    double rasterUploadTextureMs = 0.0;
    double rasterTileFinalizeMs = 0.0;
    double rasterBookkeepingMs = 0.0;
    double rasterSourceFallbackMs = 0.0;
    double rasterSourceSnapshotMs = 0.0;
    double rasterSourceIssueMs = 0.0;
    double rasterUploadQueueSelectMs = 0.0;
};

class TileUpdateUploadRunner {
public:
    template <typename ProcessPendingUploadsFn, typename MarkResourcesDirtyFn>
    static TileUpdateUploadRunResult run(
        TileUpdateUploadRunInput input,
        ProcessPendingUploadsFn&& processPendingUploads,
        MarkResourcesDirtyFn&& markResourcesDirty) {
        TileUpdateUploadRunResult result;

        const double uploadStartMs = perf::nowMs();
        const bool terrainOrContentChanged =
            processPendingUploads(
                input.interactionActive,
                input.resourceSmoothingActive,
                &input.frameResourceBudget);
        result.terrainUploadMs = perf::nowMs() - uploadStartMs;
        (void)terrainOrContentChanged;

        const double rasterUploadStartMs = perf::nowMs();
        const TileRasterOverlayUploadResult rasterUploadResult =
            TileRasterOverlayFrameProcessor::processPendingUploads(
                input.rasterOverlays,
                input.interactionActive,
                input.frameResourceBudget);
        if (rasterUploadResult.resourcesDirty) {
            markResourcesDirty();
        }
        result.rasterUploadMs = perf::nowMs() - rasterUploadStartMs;
        result.rasterUploadsProcessed = rasterUploadResult.processedUploads;
        result.rasterMappedUploadsProcessed = rasterUploadResult.mappedUploads;
        result.rasterUploadMaxMs = rasterUploadResult.maxUploadMs;
        result.rasterUploadMaxWidth = rasterUploadResult.maxUploadWidth;
        result.rasterUploadMaxHeight = rasterUploadResult.maxUploadHeight;
        result.rasterSelectTaskMs = rasterUploadResult.selectTaskMs;
        result.rasterUploadTextureMs = rasterUploadResult.uploadTextureMs;
        result.rasterTileFinalizeMs = rasterUploadResult.tileFinalizeMs;
        result.rasterBookkeepingMs = rasterUploadResult.bookkeepingMs;
        result.rasterSourceFallbackMs =
            rasterUploadResult.sourceFallbackMs;
        result.rasterSourceSnapshotMs =
            rasterUploadResult.sourceSnapshotMs;
        result.rasterSourceIssueMs =
            rasterUploadResult.sourceIssueMs;
        result.rasterUploadQueueSelectMs =
            rasterUploadResult.uploadQueueSelectMs;
        // [GPU swap 尖刺诊断] 本帧 raster 上传慢于 2ms 才打(稀少):与
        // FrameLoop swap 尖刺按时间戳关联(2026-08-21)。
        if (result.rasterUploadMs > 2.0) {
            platformLog(
                LogLevel::Info, "EarthPerf",
                "RasterUp ms=%.1f processed=%d mapped=%d",
                result.rasterUploadMs,
                result.rasterUploadsProcessed,
                result.rasterMappedUploadsProcessed);
        }

        return result;
    }

    template <typename EnsureTileFn,
              typename EnsureTileChildrenFn,
              typename MarkResourcesDirtyFn>
    static TileUpdateUploadRunResult runContentLifecycle(
        TileUpdateUploadRunInput input,
        TilesetContentUploadContext contentContext,
        EnsureTileFn&& ensureTile,
        EnsureTileChildrenFn&& ensureTileChildren,
        MarkResourcesDirtyFn&& markResourcesDirty) {
        return run(
            input,
            [&](bool uploadInteractionActive,
                bool uploadResourceSmoothingActive,
                FrameResourceBudget* budget) {
                return TilesetContentLifecycleCoordinator::processPendingUploads(
                    contentContext,
                    uploadInteractionActive,
                    uploadResourceSmoothingActive,
                    budget,
                    ensureTile,
                    ensureTileChildren,
                    markResourcesDirty);
            },
            markResourcesDirty);
    }
};

} // namespace earth_engine
