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

void TileSelectionWorker::buildShadow(
    const TilesetTileRegistry& liveRegistry) {
    // Caller guarantees !isBusy(); the acquire in isBusy() paired with the
    // worker's release of busy_ = false gives happens-before, so this runner
    // write happens-after the worker's last selection write.
    runner_.buildShadow(liveRegistry);
}

void TileSelectionWorker::dispatch(
    const TileSelectionShadowSelectInput& input) {
    std::lock_guard<std::mutex> lock(mutex_);
    job_ = input;  // value copy — no live per-frame pointers survive the call
    jobReady_ = true;
    resultReady_ = false;
    busy_.store(true, std::memory_order_release);
    cv_.notify_all();
}

const TileSelectionShadowRunner* TileSelectionWorker::tryTakeResult() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!resultReady_) {
            return nullptr;
        }
        resultReady_ = false;
    }
    // The lock acquire above happens-after the worker's result publish, so all
    // of the worker's runner_ writes are visible here.
    return &runner_;
}

void TileSelectionWorker::threadMain() {
    for (;;) {
        TileSelectionShadowSelectInput input;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this]() { return jobReady_ || stop_; });
            if (stop_ && !jobReady_) {
                return;
            }
            jobReady_ = false;
            input = job_;
        }

        runner_.selectOnShadow(input);

        {
            std::lock_guard<std::mutex> lock(mutex_);
            resultReady_ = true;
            busy_.store(false, std::memory_order_release);
            cv_.notify_all();
        }
    }
}

} // namespace earth_engine
