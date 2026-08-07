#pragma once

#include "TileLoadLifecycle.h"
#include "TilePendingLoadQueue.h"
#include "../debug/Policies.h"

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

        // 策略生效率的分母:进 terminal 循环前**本来就有多少活**(与下方
        // finalize 循环的 pendingUploadsAtEntry 同构 —— 同一函数里两条 lane,
        // 冻结形态相同,守卫也要对称,否则上半段冻结时下半段的守卫全绿)。
        size_t pendingTerminalsAtEntry = 0;
        {
            std::lock_guard<std::mutex> lock(input.lifecycle.mutex());
            pendingTerminalsAtEntry =
                input.lifecycle.pendingLoads().terminalResultCount();
        }
        int terminalsDone = 0;

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
            ++terminalsDone;

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

        // terminal 推进率:有活的帧里,有多少帧真的推进了至少一个终态结果。
        // 语义与文件末尾的 FinalizeProgress 完全一致(帧粒度二值、预算耗尽
        // 只做一部分不扣分、抓的是"有活却一个都没做"且持续如此)。
        if (pendingTerminalsAtEntry > 0) {
            policy::observe(policy::Id::TerminalStateProgress,
                            terminalsDone > 0 ? 1 : 0, 1);
        }

        // 策略生效率的分母:进 finalize 循环前**本来就有多少活**。
        size_t pendingUploadsAtEntry = 0;
        {
            std::lock_guard<std::mutex> lock(input.lifecycle.mutex());
            pendingUploadsAtEntry = input.lifecycle.pendingLoads().uploadCount();
        }
        int finalizesDone = 0;

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
            ++finalizesDone;

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

        // 策略生效率:**有活的帧里,有多少帧真的推进了至少一个 finalize**。
        //
        // 这是撤下 MainThreadFinalizeBudgetUse(已用/上限)后重新定义的版本。旧
        // 定义健康时也恒 0 —— 分母是预算上限,而空闲帧无事可做本来就是 0,与"有事
        // 却没做"读数相同。改成只在 pendingUploadsAtEntry>0 的帧上记,分母就变成
        // 「本可以推进的帧数」,冻结与空闲从此分得开。
        //
        // 逐帧二值而非比例:时间预算耗尽导致本帧只做了一部分是**正常**的,不该扣
        // 分;要抓的是"有活却一个都没做"并且**持续**如此(历史事故:交互期
        // Urgent-only 硬冻结,早退发生在 tryFinalize 之前)。窗口聚合后,
        // 偶发的 0 会被摊掉,持续冻结才会把比率压到 0。
        if (pendingUploadsAtEntry > 0) {
            policy::observe(policy::Id::FinalizeProgress,
                            finalizesDone > 0 ? 1 : 0, 1);
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
