#pragma once

#include "TileLoadLifecycle.h"
#include "TilePendingLoadQueue.h"

#include "../debug/PerfTimer.h"

#include <mutex>
#include <optional>

namespace earth_engine {

struct TilePendingLoadProcessorInput {
    TileLoadLifecycle& lifecycle;
    FrameResourceBudget& budget;
    bool interactionActive = false;
};

class TilePendingLoadProcessor {
public:
    template <typename ProcessTerrainTerminalResultFn,
              typename ProcessContentTerminalResultFn,
              typename ProcessTerrainUploadFn,
              typename ProcessContentUploadFn>
    static bool processPendingLoads(
        TilePendingLoadProcessorInput input,
        ProcessTerrainTerminalResultFn&& processTerrainTerminalResult,
        ProcessContentTerminalResultFn&& processContentTerminalResult,
        ProcessTerrainUploadFn&& processTerrainUpload,
        ProcessContentUploadFn&& processContentUpload) {
        bool changed = false;

        while (true) {
            std::optional<PendingTerrainTerminalResult> terminalResult;
            {
                std::lock_guard<std::mutex> lock(input.lifecycle.mutex());
                terminalResult =
                    input.lifecycle
                        .pendingLoads()
                        .takeHighestPriorityTerrainTerminalResult();
            }
            if (!terminalResult) {
                break;
            }

            processTerrainTerminalResult(*terminalResult);
            changed = true;
        }

        while (true) {
            std::optional<PendingContentTerminalResult> terminalResult;
            {
                std::lock_guard<std::mutex> lock(input.lifecycle.mutex());
                terminalResult =
                    input.lifecycle
                        .pendingLoads()
                        .takeHighestPriorityContentTerminalResult();
            }
            if (!terminalResult) {
                break;
            }

            processContentTerminalResult(*terminalResult);
            changed = true;
        }

        while (true) {
            std::optional<PendingLoadFinalize> finalize;

            {
                std::lock_guard<std::mutex> lock(input.lifecycle.mutex());
                finalize =
                    input.lifecycle.pendingLoads().takeHighestPriorityUpload(
                        input.interactionActive,
                        input.budget);
            }
            if (!finalize) {
                break;
            }

            const double finalizeStartMs = perf::nowMs();
            if (finalize->kind == PendingLoadFinalizeKind::Content) {
                processContentUpload(*finalize->contentUpload);
            } else {
                processTerrainUpload(*finalize->terrainUpload);
            }
            changed = true;
            input.budget.recordElapsed(
                finalize->kind == PendingLoadFinalizeKind::Content
                    ? FrameResourceLane::ContentFinalize
                    : FrameResourceLane::TerrainFinalize,
                perf::nowMs() - finalizeStartMs);
        }

        return changed;
    }
};

} // namespace earth_engine
