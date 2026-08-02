#pragma once

#include "TileLoadLifecycle.h"
#include "TilePendingLoadQueue.h"

#include "../debug/PerfTimer.h"

#include <cstdio>
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
            const double terminalElapsedMs = perf::nowMs() - terminalStartMs;
            logSlowLoadStep(
                input.budget.frameNumber(),
                "TilePendingLoad.terminal",
                terminalElapsedMs,
                *terminalResult,
                FrameResourceLane::TerminalState,
                input.interactionActive);
            const std::optional<double> overrideElapsed =
                input.elapsedOverrideMs
                    ? input.elapsedOverrideMs(FrameResourceLane::TerminalState)
                    : std::nullopt;
            input.budget.recordElapsed(
                FrameResourceLane::TerminalState,
                overrideElapsed.value_or(terminalElapsedMs));
        }

        while (true) {
            std::optional<PendingTileLoad> finalize;

            {
                std::lock_guard<std::mutex> lock(input.lifecycle.mutex());
                finalize =
                    input.lifecycle.pendingLoads().takeHighestPriorityUpload(
                        input.budget);
            }
            if (!finalize) {
                break;
            }

            const double finalizeStartMs = perf::nowMs();
            processUpload(*finalize);
            changed = true;
            const double finalizeElapsedMs = perf::nowMs() - finalizeStartMs;
            const FrameResourceLane finalizeLane =
                isContentLoadDomain(finalize->domain)
                    ? FrameResourceLane::ContentFinalize
                    : FrameResourceLane::TerrainFinalize;
            logSlowLoadStep(
                input.budget.frameNumber(),
                "TilePendingLoad.finalize",
                finalizeElapsedMs,
                *finalize,
                finalizeLane,
                input.interactionActive);
            const std::optional<double> overrideElapsed =
                input.elapsedOverrideMs
                    ? input.elapsedOverrideMs(finalizeLane)
                    : std::nullopt;
            input.budget.recordElapsed(
                finalizeLane,
                overrideElapsed.value_or(finalizeElapsedMs));
        }

        return changed;
    }

private:
    static void logSlowLoadStep(uint64_t frameNumber,
                                const char* scope,
                                double elapsedMs,
                                const PendingTileLoad& load,
                                FrameResourceLane lane,
                                bool interactionActive) {
        constexpr double kSlowLoadStepMs = 1.0;
        if (elapsedMs < kSlowLoadStepMs) {
            return;
        }
        char detail[256];
        std::snprintf(
            detail,
            sizeof(detail),
            "domain=%d lane=%d group=%d priority=%.3f cache=%s interaction=%d",
            static_cast<int>(load.domain),
            static_cast<int>(lane),
            static_cast<int>(load.group),
            load.priority,
            load.cacheKey.c_str(),
            interactionActive ? 1 : 0);
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
