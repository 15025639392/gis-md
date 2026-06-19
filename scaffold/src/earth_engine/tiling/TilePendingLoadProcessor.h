#pragma once

#include "TileLoadLifecycle.h"
#include "TilePendingLoadQueue.h"

#include "../debug/PerfTimer.h"

#include <functional>
#include <mutex>
#include <optional>

namespace earth_engine {

struct TilePendingLoadProcessorInput {
    TileLoadLifecycle& lifecycle;
    FrameResourceBudget& budget;
    bool interactionActive = false;
    std::function<std::optional<double>(FrameResourceLane)> elapsedOverrideMs;
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
            std::optional<PendingTerminalResult> terminalResult;
            {
                std::lock_guard<std::mutex> lock(input.lifecycle.mutex());
                terminalResult =
                    input.lifecycle
                        .pendingLoads()
                        .takeHighestPriorityTerminalResult(input.budget);
            }
            if (!terminalResult) {
                break;
            }

            const double terminalStartMs = perf::nowMs();
            if (terminalResult->kind == PendingTerminalResultKind::Content) {
                processContentTerminalResult(
                    *terminalResult->contentResult);
            } else {
                processTerrainTerminalResult(
                    *terminalResult->terrainResult);
            }
            changed = true;
            const std::optional<double> overrideElapsed =
                input.elapsedOverrideMs
                    ? input.elapsedOverrideMs(FrameResourceLane::TerminalState)
                    : std::nullopt;
            input.budget.recordElapsed(
                FrameResourceLane::TerminalState,
                overrideElapsed.value_or(perf::nowMs() - terminalStartMs));
        }

        while (true) {
            std::optional<PendingLoadFinalize> finalize;

            {
                std::lock_guard<std::mutex> lock(input.lifecycle.mutex());
                finalize =
                    input.lifecycle.pendingLoads().takeHighestPriorityUpload(
                        PendingLoadFinalizeContext{
                            input.interactionActive,
                            input.budget});
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
            const FrameResourceLane finalizeLane =
                finalize->kind == PendingLoadFinalizeKind::Content
                    ? FrameResourceLane::ContentFinalize
                    : FrameResourceLane::TerrainFinalize;
            const std::optional<double> overrideElapsed =
                input.elapsedOverrideMs
                    ? input.elapsedOverrideMs(finalizeLane)
                    : std::nullopt;
            input.budget.recordElapsed(
                finalizeLane,
                overrideElapsed.value_or(perf::nowMs() - finalizeStartMs));
        }

        return changed;
    }
};

} // namespace earth_engine
