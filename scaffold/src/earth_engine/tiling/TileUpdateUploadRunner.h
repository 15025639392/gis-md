#pragma once

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

        return result;
    }
};

} // namespace earth_engine
