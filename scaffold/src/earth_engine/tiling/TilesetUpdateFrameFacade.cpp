#include "TilesetUpdateFrameFacade.h"

#include "TileFrameDebugLogFormatter.h"
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

    const TilesetUpdateFrameRuntimeResult updateResult =
        TilesetUpdateFrameRuntime::run(tileset, frameState);

    const std::array<char, 640> updateDetail =
        TileFrameDebugLogFormatter::updateDetail(
            updateResult.debugLog);
    perf::logTimingAtLeast(frameState.frameId,
                           "Tileset.update",
                           perf::nowMs() - updateStartMs,
                           10.0,
                           updateDetail.data());
}

} // namespace earth_engine
