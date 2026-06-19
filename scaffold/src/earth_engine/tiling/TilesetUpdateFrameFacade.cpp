#include "TilesetUpdateFrameFacade.h"

#include "TileContentLifecycleManager.h"
#include "TileFrameDebugLogFormatter.h"
#include "Tileset.h"
#include "TilesetUpdateFrameRuntime.h"

#include "../debug/PerfTimer.h"
#include "../scene/FrameState.h"

#include <array>

namespace earth_engine {

void TilesetUpdateFrameFacade::update(
    Tileset& tileset,
    const FrameState& frameState) {
    if (!frameState.camera) return;
    const double updateStartMs = perf::nowMs();

    // cesium-native: increment generation each frame so that
    // RenderCommand validator (non-zero check) accepts SurfaceTile commands.
    ++tileset.generation_;

    const TileFrameWorkResult frameWork =
        TilesetUpdateFrameRuntime::run(tileset, frameState);
    const TileUpdateUploadRunResult& uploadWork = frameWork.uploadWork;
    const TileUpdateSelectionWorkResult& selectionWork =
        frameWork.selectionWork;

    const std::array<char, 384> updateDetail =
        TileFrameDebugLogFormatter::updateDetail(
            TileUpdateDebugLogInput{
                tileset.tilePlan_.visibleTiles.size(),
                tileset.loadQueue_.size(),
                selectionWork.computeMs,
                selectionWork.prefetchMs,
                selectionWork.requestMs,
                uploadWork.terrainUploadMs,
                uploadWork.rasterUploadMs,
                tileset.contentLifecycle_.terrainCache().size(),
                tileset.contentLifecycle_.loadLifecycle()
                    .requestState()
                    .totalRequestCount(),
                tileset.selectionCounters_,
                selectionWork.reuseMode,
                selectionWork.reuseRejectReason,
                selectionWork.reusedSelection,
                uploadWork.rasterUploadsProcessed,
                frameWork.interactionActive,
                frameWork.resourceSmoothingActive});
    perf::logTimingAtLeast(frameState.frameId,
                           "Tileset.update",
                           perf::nowMs() - updateStartMs,
                           10.0,
                           updateDetail.data());
}

} // namespace earth_engine
