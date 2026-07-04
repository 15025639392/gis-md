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
    Destroying
};

class TileLoadRequestDispatcher {
public:
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
                return TileLoadDispatchResult::Skipped;
            }
            if (!budget.tryIssue(
                    requestLane,
                    TileLoadPriorityPolicy::toFramePriority(group),
                    estimatedFanout)) {
                return TileLoadDispatchResult::Blocked;
            }
            if (!requestState.beginContentRequest(cacheKey, token)) {
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
                    if (!requestState.destroying() && !token.isCancelled()) {
                        TileLoadResult loadResult =
                            TileLoadDomainPolicy::normalizeForDomain(
                            domain,
                            TileLoadResult::fromContentResult(
                                std::move(result)));
                        enqueueCompletedLoadResult(
                            pendingLoads,
                            domain,
                            key,
                            cacheKey,
                            group,
                            priority,
                            std::move(loadResult));
                    }
                    requestState.completeContentRequest(cacheKey);
                }
                condition.notify_all();
            };
        auto guard = std::make_shared<ContentCompletionGuard>(
            std::move(complete));
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

    // 上采样 clip 的异步派发,结构与 requestContent 一比一对齐:主线程认领
    // (beginTerrainRequest 占 in-flight 名额、堵 containsWorkForCacheKey 去重、
    // 计入析构等待集)+ budget.tryIssue 门控派发数 → 把纯 CPU 的 clip 工作
    // 丢进 AsyncSystem::pool worker → 完成回调单锁经 enqueueCompletedLoadResult
    // 投 pendingLoads(与 requestContent 复用同一 worker→lifecycle 锁→pendingLoads
    // 模型)。clipInput 是主线程建好的自有快照(零 TilesetTile 指针),clip
    // 在 worker 只读快照,不碰共享可变状态。UpsampleClipCompletionGuard 保证
    // 完成 exactly-once(worker 若被抛弃仍以失败终态完成),否则 in-flight 名额
    // 泄漏 → markDestroyingCancelAndWait 永久阻塞死锁。
    template <typename OnIssuedFn, typename ClipInput, typename ClipFn>
    static TileLoadDispatchResult requestUpsampleClip(
        std::mutex& mutex,
        std::condition_variable& condition,
        TilePendingRequestState& requestState,
        TilePendingLoadQueue& pendingLoads,
        FrameResourceBudget& budget,
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
                return TileLoadDispatchResult::Skipped;
            }
            if (!budget.tryIssue(
                    FrameResourceLane::TerrainRequest,
                    TileLoadPriorityPolicy::toFramePriority(group),
                    1)) {
                return TileLoadDispatchResult::Blocked;
            }
            if (!requestState.beginTerrainRequest(cacheKey, token)) {
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
                    if (!requestState.destroying() && !token.isCancelled()) {
                        TileLoadResult normalized =
                            TileLoadDomainPolicy::normalizeForDomain(
                                TileLoadDomain::TerrainContent,
                                std::move(result));
                        enqueueCompletedLoadResult(
                            pendingLoads,
                            TileLoadDomain::TerrainContent,
                            key,
                            cacheKey,
                            group,
                            priority,
                            std::move(normalized));
                    }
                    requestState.completeTerrainRequest(cacheKey);
                }
                condition.notify_all();
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

private:
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
};

} // namespace earth_engine
