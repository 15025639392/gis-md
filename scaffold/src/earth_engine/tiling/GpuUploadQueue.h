#pragma once

#include "GpuReadyData.h"
#include "TileKey.h"

#include <deque>
#include <algorithm>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

namespace earth_engine {

/// A pending GPU upload: the tile it belongs to + the CPU-prepared data.
struct PendingGpuUpload {
    TileKey tileKey;
    std::string cacheKey;  // lifecycle cache key, for eraseUpload
    GpuReadyData data;
};

/// Thread-safe queue for passing CPU-prepared data from worker threads
/// to the main thread for GPU upload.
class GpuUploadQueue {
public:
    /// Worker thread pushes CPU-prepared data.
    void push(PendingGpuUpload&& upload) {
        std::lock_guard<std::mutex> lock(mutex_);
        pendingBytes_ += std::max<int64_t>(0, upload.data.byteSize());
        queue_.push_back(std::move(upload));
    }

    /// Main thread pops the highest-priority upload (FIFO).
    std::optional<PendingGpuUpload> tryPop() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) return std::nullopt;
        pendingBytes_ = std::max<int64_t>(
            0,
            pendingBytes_ - queue_.front().data.byteSize());
        PendingGpuUpload upload = std::move(queue_.front());
        queue_.pop_front();
        return upload;
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
                    return true;
                }),
            queue_.end());
        return before - queue_.size();
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.clear();
        pendingBytes_ = 0;
    }

private:
    mutable std::mutex mutex_;
    std::deque<PendingGpuUpload> queue_;
    int64_t pendingBytes_ = 0;
};

} // namespace earth_engine
