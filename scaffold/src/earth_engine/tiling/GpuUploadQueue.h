#pragma once

#include "GpuReadyData.h"
#include "TileKey.h"

#include "../debug/Contracts.h"

#include <deque>
#include <algorithm>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_set>

namespace earth_engine {

/// A pending GPU upload: the tile it belongs to + the CPU-prepared data.
struct PendingGpuUpload {
    TileKey tileKey;
    std::string cacheKey;  // lifecycle cache key, for eraseUpload
    GpuReadyData data;
    /// 入队序号(由 GpuUploadQueue::push 打)。仅供 FIFO 契约比对,业务逻辑不读。
    uint64_t enqueueSeq = 0;
};

/// 线程安全队列,承载「CPU 侧已备好的字节」到「建 GPU buffer」之间的一跳。
///
/// ⚠️ 实际线程归属(2026-08-06 复核,与旧注释所写不同):push 与 tryPop **都在主
/// 线程**,且在同一帧内 —— push 发生在 Tileset::update 的 processPendingUploads
/// (经 TilePendingLoadCommitCoordinator),pop 发生在同帧稍后的
/// drainGpuUploadQueue。真正在 worker 上完成的是更早的顶点转换(写
/// GltfPrimitive::terrainGpuVertexBytes),队列只搬运那批已做好的字节。互斥锁保护
/// 的是 eraseCacheKey/clear 这些来自卸载路径的并发访问,不是 worker 生产者。
///
/// 因此这一跳换来的**唯一实际收益**是 drain 侧那道每帧上传闸(maxUploadsPerFrame
/// 默认 4,交互期地板 2 / 空闲地板 4):超出部分自然溢到下一帧。同步路没有这层。
///
/// 存在条件:入队闸要求 primitive 同时具备 hasTerrainWaterMaskMetadata 与完整的
/// terrainGpuVertexBytes,这两个字段唯一的生产者在上采样器路径里(见
/// GltfTerrainUpsampler.h 的存活条件与 contracts::Gate::ImageryDrivenUpsample)。
/// 生产默认 decoupleImageryFromGeometry=true → 上采样不跑 → 本队列恒空;
/// TilesetOptions 的结构体默认是 false,那种配置下本队列**是活的**(真机对照:
/// decouple=false 时 FIFO 契约 coverage=152)。这是配置门控,不是待接线的半成品。
class GpuUploadQueue {
public:
    /// A dequeue that has not yet crossed frame admission.  The token is
    /// owned by this queue and must be resolved exactly once with commitPop
    /// or requeue.  Keeping the token next to the payload makes the former
    /// "tryPop then immediately pushFront" call-order convention explicit.
    class ProvisionalPop : public PendingGpuUpload {
    public:
        ProvisionalPop(ProvisionalPop&&) noexcept = default;
        ProvisionalPop& operator=(ProvisionalPop&&) noexcept = default;
        ProvisionalPop(const ProvisionalPop&) = delete;
        ProvisionalPop& operator=(const ProvisionalPop&) = delete;

    private:
        friend class GpuUploadQueue;
        ProvisionalPop(PendingGpuUpload&& value, uint64_t token)
            : PendingGpuUpload(std::move(value)), token_(token) {}

        uint64_t token_ = 0;
    };

    /// 入队(当前调用点在主线程,见类注释)。
    void push(PendingGpuUpload&& upload) {
        std::lock_guard<std::mutex> lock(mutex_);
        pendingBytes_ += std::max<int64_t>(0, upload.data.byteSize());
        upload.enqueueSeq = ++enqueueSeq_;
        queue_.push_back(std::move(upload));
    }

    /// 出队,严格 FIFO(无优先级重排)。
    std::optional<PendingGpuUpload> tryPop() {
        std::lock_guard<std::mutex> lock(mutex_);
        GE_CONTRACT(contracts::Id::GpuUploadQueueFifo,
                    activeProvisionalToken_ == 0,
                    "committed pop while provisional token=%llu is active",
                    (unsigned long long)activeProvisionalToken_);
        if (activeProvisionalToken_ != 0) return std::nullopt;
        if (queue_.empty()) return std::nullopt;
        pendingBytes_ = std::max<int64_t>(
            0,
            pendingBytes_ - queue_.front().data.byteSize());
        PendingGpuUpload upload = std::move(queue_.front());
        queue_.pop_front();
        // 契约(消费侧):严格 FIFO —— 取出的入队序号必须正好是下一个
        // 未被 erase/clear 取消的序号。
        //
        // 谓词与队列实现**数据独立**:这里比的是独立维护的出队计数与 push 时打上
        // 的序号,不是"再检查一遍 push_back/pop_front 写对了没有"。换成
        // push_front、加优先级重排、或者哪天把 deque 换成堆,都会当场被抓。
        //
        // AI_INDEX §20 把 FIFO 列为跨子系统契约,但那一节自己写着 "enforced by
        // call order, not by types" —— 在此之前它只是一句文档。
        advancePastErasedLocked();
        const uint64_t expectedSeq = dequeueSeq_ + 1;
        GE_CONTRACT(contracts::Id::GpuUploadQueueFifo,
                    upload.enqueueSeq == expectedSeq,
                    "expectedSeq=%llu gotSeq=%llu queueSize=%zu tile=%d/%d/%d",
                    (unsigned long long)expectedSeq,
                    (unsigned long long)upload.enqueueSeq,
                    queue_.size(),
                    upload.tileKey.z, upload.tileKey.x, upload.tileKey.y);
        dequeueSeq_ = std::max(dequeueSeq_, upload.enqueueSeq);
        advancePastErasedLocked();
        return upload;
    }

    /// Remove the FIFO head provisionally.  Only one provisional item may be
    /// outstanding per queue, which matches the render-thread drain loop and
    /// prevents an unrelated payload from being pushed to the front.
    std::optional<ProvisionalPop> tryPopProvisional() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) return std::nullopt;
        GE_CONTRACT(contracts::Id::GpuUploadQueueFifo,
                    activeProvisionalToken_ == 0,
                    "nested provisional pop token=%llu queueSize=%zu",
                    (unsigned long long)activeProvisionalToken_,
                    queue_.size());
        if (activeProvisionalToken_ != 0) {
            return std::nullopt;
        }

        pendingBytes_ = std::max<int64_t>(
            0,
            pendingBytes_ - queue_.front().data.byteSize());
        PendingGpuUpload upload = std::move(queue_.front());
        queue_.pop_front();
        advancePastErasedLocked();
        const uint64_t expectedSeq = dequeueSeq_ + 1;
        GE_CONTRACT(contracts::Id::GpuUploadQueueFifo,
                    upload.enqueueSeq == expectedSeq,
                    "expectedSeq=%llu gotSeq=%llu queueSize=%zu tile=%d/%d/%d",
                    (unsigned long long)expectedSeq,
                    (unsigned long long)upload.enqueueSeq,
                    queue_.size(),
                    upload.tileKey.z, upload.tileKey.x, upload.tileKey.y);
        activeProvisionalToken_ = ++nextProvisionalToken_;
        activeProvisionalSeq_ = upload.enqueueSeq;
        return ProvisionalPop(
            std::move(upload), activeProvisionalToken_);
    }

    /// Permanently consume a provisionally popped item after admission.
    bool commitPop(ProvisionalPop& pop) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!validProvisionalLocked(pop)) {
            return false;
        }
        dequeueSeq_ = std::max(dequeueSeq_, activeProvisionalSeq_);
        advancePastErasedLocked();
        clearProvisionalLocked(pop);
        return true;
    }

    /// Restore a provisionally popped item after frame admission denial.
    bool requeue(ProvisionalPop&& pop) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!validProvisionalLocked(pop)) {
            return false;
        }
        pendingBytes_ += std::max<int64_t>(0, pop.data.byteSize());
        queue_.push_front(std::move(static_cast<PendingGpuUpload&>(pop)));
        clearProvisionalLocked(pop);
        return true;
    }

    /// Check if there are pending uploads (non-blocking).
    bool hasWork() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return !queue_.empty();
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

    int64_t pendingBytes() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return pendingBytes_;
    }

    size_t eraseCacheKey(const std::string& cacheKey) {
        std::lock_guard<std::mutex> lock(mutex_);
        const size_t before = queue_.size();
        queue_.erase(
            std::remove_if(
                queue_.begin(),
                queue_.end(),
                [&](const PendingGpuUpload& upload) {
                    if (upload.cacheKey != cacheKey) {
                        return false;
                    }
                    pendingBytes_ = std::max<int64_t>(
                        0,
                        pendingBytes_ - upload.data.byteSize());
                    erasedSeqs_.insert(upload.enqueueSeq);
                    return true;
                }),
            queue_.end());
        advancePastErasedLocked();
        return before - queue_.size();
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.clear();
        pendingBytes_ = 0;
        dequeueSeq_ = enqueueSeq_;
        erasedSeqs_.clear();
        activeProvisionalToken_ = 0;
        activeProvisionalSeq_ = 0;
    }

private:
    void advancePastErasedLocked() {
        while (erasedSeqs_.erase(dequeueSeq_ + 1) != 0) {
            ++dequeueSeq_;
        }
    }

    bool validProvisionalLocked(const ProvisionalPop& pop) const {
        const bool valid = pop.token_ != 0 &&
                           pop.token_ == activeProvisionalToken_ &&
                           pop.enqueueSeq == activeProvisionalSeq_;
        GE_CONTRACT(contracts::Id::GpuUploadQueueFifo,
                    valid,
                    "invalid provisional token expected=%llu got=%llu expectedSeq=%llu gotSeq=%llu",
                    (unsigned long long)activeProvisionalToken_,
                    (unsigned long long)pop.token_,
                    (unsigned long long)activeProvisionalSeq_,
                    (unsigned long long)pop.enqueueSeq);
        return valid;
    }

    void clearProvisionalLocked(ProvisionalPop& pop) {
        pop.token_ = 0;
        activeProvisionalToken_ = 0;
        activeProvisionalSeq_ = 0;
    }

    mutable std::mutex mutex_;
    std::deque<PendingGpuUpload> queue_;
    int64_t pendingBytes_ = 0;
    // FIFO 契约用的两个独立计数(都在 mutex_ 保护下)。业务逻辑不读它们 ——
    // 它们的全部作用就是让"严格 FIFO"这句话可判真假。
    uint64_t enqueueSeq_ = 0;
    uint64_t dequeueSeq_ = 0;
    std::unordered_set<uint64_t> erasedSeqs_;
    uint64_t nextProvisionalToken_ = 0;
    uint64_t activeProvisionalToken_ = 0;
    uint64_t activeProvisionalSeq_ = 0;
};

} // namespace earth_engine
