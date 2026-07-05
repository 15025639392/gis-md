#include "TileSelectionWorker.h"

#include "RasterMappedToTilesetTile.h"
#include "TilesetTile.h"

namespace earth_engine {

TileSelectionWorker::TileSelectionWorker() {
    thread_ = std::thread([this]() { threadMain(); });
}

TileSelectionWorker::~TileSelectionWorker() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
        cv_.notify_all();
    }
    if (thread_.joinable()) {
        thread_.join();
    }
}

const TileSelectionShadowRunner& TileSelectionWorker::runSync(
    const TileSelectionShadowRunInput& input) {
    std::unique_lock<std::mutex> lock(mutex_);
    pendingInput_ = &input;
    jobReady_ = true;
    resultReady_ = false;
    cv_.notify_all();
    // Barrier: block the render thread until the worker publishes the result.
    cv_.wait(lock, [this]() { return resultReady_; });
    pendingInput_ = nullptr;
    return runner_;
}

void TileSelectionWorker::threadMain() {
    for (;;) {
        const TileSelectionShadowRunInput* input = nullptr;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this]() { return jobReady_ || stop_; });
            if (stop_ && !jobReady_) {
                return;
            }
            jobReady_ = false;
            input = pendingInput_;
        }

        // Run the selection outside the lock (the render thread is barrier-
        // blocked in runSync, so `runner_` and the referenced live state have no
        // concurrent access).
        if (input) {
            runner_.run(*input);
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            resultReady_ = true;
            cv_.notify_all();
        }
    }
}

} // namespace earth_engine
