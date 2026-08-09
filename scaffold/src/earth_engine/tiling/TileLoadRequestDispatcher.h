#pragma once

#include "TileLoadDomainPolicy.h"
#include "TileLoadTypes.h"
#include "TilePendingLoadQueue.h"
#include "TilePendingRequestState.h"
#include "TileLoadPriorityPolicy.h"
#include "../core/async/AsyncSystem.h"
#include "../core/resources/FrameResourceBudget.h"
#include "../content/GltfContentProvider.h"
#include "../threading/CancellationToken.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

namespace earth_engine {

enum class TileLoadDispatchResult {
    Issued,
    Skipped,
    Blocked,
    WorkerCapacityBlocked,
    Destroying
};

class TileLoadRequestDispatcher {
public:
    static size_t maximumUpsampleClipInflight() {
        return std::max<size_t>(
            1u,
            AsyncSystem::pool().threadCount() / 2u);
    }

    static bool hasUpsampleClipWorkerCapacity() {
        return activeUpsampleClipTasks_.load(std::memory_order_relaxed) <
               maximumUpsampleClipInflight();
    }

    static TileLoadDispatchResult queueUpsampledLoad(
        std::mutex& mutex,
        TilePendingRequestState& requestState,
        TilePendingLoadQueue& pendingLoads,
        const TileKey& key,
        const std::string& cacheKey,
        TileLoadPriorityGroup group,
        double priority,
        TileLoadDomain domain,
        TileLoadResult result);

    template <typename OnIssuedFn>
    static TileLoadDispatchResult requestContent(
        std::mutex& mutex,
        std::condition_variable& condition,
        TilePendingRequestState& requestState,
        TilePendingLoadQueue& pendingLoads,
        FrameResourceBudget& budget,
        TilesetContentProvider& provider,
        const TileKey& key,
        const std::string& cacheKey,
        TileLoadPriorityGroup group,
        double priority,
        OnIssuedFn&& onIssued,
        TileContentRequestOptions options = {}) {
        CancellationToken token;
        // 动态优先级 cell:注册进 requestState(markNeeded 随帧写)并透传到
        // HTTP 层(curl 工作线程读它搬桶)。在飞请求的优先级自此不再冻结。
        auto priorityCell = std::make_shared<std::atomic<int>>(
            static_cast<int>(toHttpPriority(group)));
        const int estimatedFanout = provider.estimatedRequestFanout(key);
        const FrameResourceLane requestLane =
            provider.providesTerrainQuadtree()
                ? FrameResourceLane::TerrainRequest
                : FrameResourceLane::ContentRequest;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (requestState.destroying()) {
                return TileLoadDispatchResult::Destroying;
            }
            if (cacheKey.empty()) {
                return TileLoadDispatchResult::Skipped;
            }
            if (requestState.contains(cacheKey) ||
                pendingLoads.containsCacheKey(cacheKey)) {
                // 去重命中 = 这个在飞 key 本帧仍被要(上采样源等经由本口
                // 逐帧重试的路径全靠这行续命,否则 30 帧后被差集回收误杀);
                // 同时把最新 group 写进动态优先级 cell(promotion 重排)。
                requestState.markNeeded(
                    cacheKey,
                    static_cast<int>(toHttpPriority(group)));
                return TileLoadDispatchResult::Skipped;
            }
            if (!budget.tryIssue(
                    requestLane,
                    TileLoadPriorityPolicy::toFramePriority(group),
                    estimatedFanout)) {
                return TileLoadDispatchResult::Blocked;
            }
            if (!requestState.beginContentRequest(cacheKey, token,
                                                  priorityCell)) {
                return TileLoadDispatchResult::Skipped;
            }
        }

        onIssued();
        const TileLoadDomain domain =
            provider.providesTerrainQuadtree()
                ? TileLoadDomain::TerrainContent
                : TileLoadDomain::Content;
        // The in-flight gate (requestState) and the destroy-time wait in
        // TileLoadLifecycle::markDestroyingCancelAndWait both require this
        // completion to run exactly once on every path. cesium-native gets
        // that from guaranteed Future continuations; here a guard completes
        // with a failed result if the provider drops the callback without
        // invoking it (otherwise the key leaks, the gate blocks forever and
        // engine destruction deadlocks).
        auto complete =
            [&mutex,
             &condition,
             &requestState,
             &pendingLoads,
             cacheKey,
             key,
             token,
             group,
             priority,
             domain](TileContentLoadResult result) mutable {
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    // ⚠️取消**也必须入队一个终态**。此前取消是直接丢结果的,
                    // 于是没有任何东西把瓦片从 ContentLoading 推出去:请求侧
                    // 记账被下面的 completeContentRequest 清掉(pending 归 0,
                    // 引擎据此报"已收敛"),瓦片侧却永远停在加载中,调度器看它
                    // "还在加载"就不再请求 —— 永久卡死且零报错。
                    // TileTerminalLoadPolicy 里的 case Cancelled(→
                    // markUnknownTemporaryFailure,退避后重试)本就是为这条
                    // 路径写的,只是从来没被走到过。
                    // 真机(2026-08-09,25000m 冷启动):stale 差集回收一次取消
                    // 35~54 个地形请求 → registry 里 80 块瓦片恒在
                    // ContentLoading、failTemp 恒 0、此后再不发一个请求,
                    // 屏幕上地形与影像都没上屏。
                    // destroying 仍是硬闸:销毁期不该再往队列里放东西。
                    const bool staleCancelled =
                        requestState.takeStaleCancelled(cacheKey);
                    if (!requestState.destroying() &&
                        (!token.isCancelled() || staleCancelled)) {
                        TileLoadResult loadResult =
                            TileLoadDomainPolicy::normalizeForDomain(
                            domain,
                            TileLoadResult::fromContentResult(
                                token.isCancelled()
                                    ? TileContentLoadResult::cancelled()
                                    : std::move(result)));
                        enqueueCompletedLoadResult(
                            pendingLoads,
                            domain,
                            key,
                            cacheKey,
                            group,
                            priority,
                            std::move(loadResult));
                    }
                    requestState.completeContentRequest(
                        cacheKey,
                        token);
                    condition.notify_all();
                }
            };
        auto guard = std::make_shared<ContentCompletionGuard>(
            std::move(complete));
        options.httpPriorityCell = priorityCell;
        provider.requestTileContent(
            key,
            token,
            [guard](const TileKey&, TileContentLoadResult result) mutable {
                guard->fire(std::move(result));
            },
            toHttpPriority(group),
            options);
        return TileLoadDispatchResult::Issued;
    }

    // 上采样 clip 是本地 CPU 工作，不占网络 budget。主线程仍通过
    // beginTerrainRequest 认领 key，用于去重、取消和析构等待；worker 完成后
    // 经 pendingLoads 回到统一提交路径。clipInput 是主线程建好的自有快照
    // (零 TilesetTile 指针)，worker 只读快照，不碰共享可变状态。
    // UpsampleClipCompletionGuard 保证 exactly-once，否则 lifecycle 名额泄漏
    // 会令 markDestroyingCancelAndWait 永久阻塞。
    template <typename OnIssuedFn, typename ClipInput, typename ClipFn>
    static TileLoadDispatchResult requestUpsampleClip(
        std::mutex& mutex,
        std::condition_variable& condition,
        TilePendingRequestState& requestState,
        TilePendingLoadQueue& pendingLoads,
        const TileKey& key,
        const std::string& cacheKey,
        TileLoadPriorityGroup group,
        double priority,
        ClipInput clipInput,
        ClipFn clip,
        OnIssuedFn onIssued) {
        CancellationToken token;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (requestState.destroying()) {
                return TileLoadDispatchResult::Destroying;
            }
            if (cacheKey.empty()) {
                return TileLoadDispatchResult::Skipped;
            }
            if (requestState.contains(cacheKey) ||
                pendingLoads.containsCacheKey(cacheKey)) {
                // 去重命中 = 这个在飞 key 本帧仍被要(上采样源等经由本口
                // 逐帧重试的路径全靠这行续命,否则 30 帧后被差集回收误杀);
                // 同时把最新 group 写进动态优先级 cell(promotion 重排)。
                requestState.markNeeded(
                    cacheKey,
                    static_cast<int>(toHttpPriority(group)));
                return TileLoadDispatchResult::Skipped;
            }
            if (!tryAcquireUpsampleClipSlot()) {
                return TileLoadDispatchResult::WorkerCapacityBlocked;
            }
            if (!requestState.beginTerrainRequest(cacheKey, token)) {
                releaseUpsampleClipSlot();
                return TileLoadDispatchResult::Skipped;
            }
        }

        onIssued();
        auto complete =
            [&mutex,
             &condition,
             &requestState,
             &pendingLoads,
             cacheKey,
             key,
             token,
             group,
             priority](TileLoadResult result) mutable {
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    // 同上(内容域那条注释):取消必须落终态,否则瓦片永久
                    // 停在 ContentLoading。
                    const bool staleCancelled =
                        requestState.takeStaleCancelled(cacheKey);
                    if (!requestState.destroying() &&
                        (!token.isCancelled() || staleCancelled)) {
                        TileLoadResult normalized =
                            TileLoadDomainPolicy::normalizeForDomain(
                                TileLoadDomain::TerrainContent,
                                token.isCancelled()
                                    ? TileLoadResult::createTerminal(
                                          TileLoadStatus::Cancelled)
                                    : std::move(result));
                        enqueueCompletedLoadResult(
                            pendingLoads,
                            TileLoadDomain::TerrainContent,
                            key,
                            cacheKey,
                            group,
                            priority,
                            std::move(normalized));
                    }
                    releaseUpsampleClipSlot();
                    requestState.completeTerrainRequest(
                        cacheKey,
                        token);
                    condition.notify_all();
                }
            };
        auto guard = std::make_shared<UpsampleClipCompletionGuard>(
            std::move(complete));
        AsyncSystem::pool().enqueue(
            [guard,
             clip = std::move(clip),
             clipInput = std::move(clipInput),
             token]() mutable {
                if (token.isCancelled()) {
                    guard->fire(
                        TileLoadResult::createTerminal(TileLoadStatus::Failed));
                    return;
                }
                guard->fire(clip(clipInput));
            });
        return TileLoadDispatchResult::Issued;
    }

    /// group → HTTP 三档映射。public:调度器去重点/差集回收也要用它
    /// 更新在飞请求的动态优先级 cell。
    static HttpRequestPriority toHttpPriority(TileLoadPriorityGroup group) {
        switch (group) {
            case TileLoadPriorityGroup::Preload:
                return HttpRequestPriority::Low;
            case TileLoadPriorityGroup::Normal:
                return HttpRequestPriority::Normal;
            case TileLoadPriorityGroup::Urgent:
                return HttpRequestPriority::High;
        }
        return HttpRequestPriority::Normal;
    }

private:
    static bool tryAcquireUpsampleClipSlot() {
        size_t active =
            activeUpsampleClipTasks_.load(std::memory_order_relaxed);
        const size_t maximum = maximumUpsampleClipInflight();
        while (active < maximum) {
            if (activeUpsampleClipTasks_.compare_exchange_weak(
                    active,
                    active + 1u,
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed)) {
                return true;
            }
        }
        return false;
    }

    static void releaseUpsampleClipSlot() {
        activeUpsampleClipTasks_.fetch_sub(
            1u,
            std::memory_order_acq_rel);
    }

    inline static std::atomic<size_t> activeUpsampleClipTasks_{0};

    /// TileLoadResult 版完成 guard:上采样 clip worker 完成 exactly-once;若
    /// worker 被抛弃(池停机)而从未 fire,析构以失败终态兜底,防 in-flight
    /// 名额泄漏 → 析构等待死锁。语义同 ContentCompletionGuard。
    class UpsampleClipCompletionGuard {
    public:
        explicit UpsampleClipCompletionGuard(
            std::function<void(TileLoadResult)> complete)
            : complete_(std::move(complete)) {}
        UpsampleClipCompletionGuard(const UpsampleClipCompletionGuard&) =
            delete;
        UpsampleClipCompletionGuard& operator=(
            const UpsampleClipCompletionGuard&) = delete;
        ~UpsampleClipCompletionGuard() {
            if (complete_) {
                fire(TileLoadResult::createTerminal(TileLoadStatus::Failed));
            }
        }
        void fire(TileLoadResult result) {
            if (!complete_) {
                return;
            }
            auto fn = std::move(complete_);
            complete_ = nullptr;
            fn(std::move(result));
        }

    private:
        std::function<void(TileLoadResult)> complete_;
    };

    /// Invokes the wrapped completion exactly once: either with the
    /// provider's real result, or with a failed result from the destructor
    /// when the provider destroys the callback without ever calling it.
    class ContentCompletionGuard {
    public:
        explicit ContentCompletionGuard(
            std::function<void(TileContentLoadResult)> complete)
            : complete_(std::move(complete)) {}
        ContentCompletionGuard(const ContentCompletionGuard&) = delete;
        ContentCompletionGuard& operator=(const ContentCompletionGuard&) =
            delete;
        ~ContentCompletionGuard() {
            if (complete_) {
                fire(TileContentLoadResult::failed());
            }
        }
        void fire(TileContentLoadResult result) {
            if (!complete_) {
                return;
            }
            auto fn = std::move(complete_);
            complete_ = nullptr;
            fn(std::move(result));
        }

    private:
        std::function<void(TileContentLoadResult)> complete_;
    };

    static void enqueueCompletedLoadResult(
        TilePendingLoadQueue& pendingLoads,
        TileLoadDomain domain,
        const TileKey& key,
        const std::string& cacheKey,
        TileLoadPriorityGroup group,
        double priority,
        TileLoadResult loadResult) {
        PendingTileLoad pending{
            domain,
            key,
            cacheKey,
            group,
            priority,
            std::move(loadResult)};
        if (pending.result.shouldUpload()) {
            pendingLoads.addUpload(std::move(pending));
        } else {
            pendingLoads.addTerminalResult(std::move(pending));
        }
    }

};

} // namespace earth_engine
