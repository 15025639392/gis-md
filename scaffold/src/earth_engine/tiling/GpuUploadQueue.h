#pragma once

#include "GpuReadyData.h"
#include "TileKey.h"
#include "TilesetTile.h"

#include <deque>
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
        queue_.push_back(std::move(upload));
    }

    /// Main thread pops the highest-priority upload (FIFO).
    std::optional<PendingGpuUpload> tryPop() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) return std::nullopt;
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

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.clear();
    }

private:
    mutable std::mutex mutex_;
    std::deque<PendingGpuUpload> queue_;
};

} // namespace earth_engine