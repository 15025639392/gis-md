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
    template <typename ProcessTerminalResultFn, typename ProcessUploadFn>
    static bool processPendingLoads(
        TilePendingLoadProcessorInput input,
        ProcessTerminalResultFn&& processTerminalResult,
        ProcessUploadFn&& processUpload) {
        bool changed = false;

        while (true) {
            std::optional<PendingTileLoad> terminalResult;
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
            processTerminalResult(*terminalResult);
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
            std::optional<PendingTileLoad> finalize;

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
            processUpload(*finalize);
            changed = true;
            const FrameResourceLane finalizeLane =
                isContentLoadDomain(finalize->domain)
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
