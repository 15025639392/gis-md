#include "TileSelectionWorker.h"

#include "RasterMappedToTilesetTile.h"
#include "TilesetTile.h"

#include <cassert>

namespace earth_engine {

TileSelectionWorker::TileSelectionWorker()
    : renderThreadId_(std::this_thread::get_id()) {
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
    assert(std::this_thread::get_id() == renderThreadId_ &&
           "buildShadow must run on the render thread");
    assert(!busy_.load(std::memory_order_acquire) &&
           "buildShadow while the worker is selecting corrupts the shadow");
    runner_.buildShadow(liveRegistry);
}

void TileSelectionWorker::dispatch(
    const TileSelectionShadowSelectInput& input) {
    assert(std::this_thread::get_id() == renderThreadId_ &&
           "dispatch must run on the render thread");
    assert(!busy_.load(std::memory_order_acquire) &&
           "dispatch while the worker is busy overwrites the running job");
    // The worker reads live mutable occlusion state through this thunk, which
    // the render thread updates — it MUST be null on the async path.
    assert(input.checkOcclusion == nullptr &&
           "async worker must not carry a live-reading occlusion thunk");
    std::lock_guard<std::mutex> lock(mutex_);
    job_ = input;  // value copy — no live per-frame pointers survive the call
    jobReady_ = true;
    resultReady_ = false;
    busy_.store(true, std::memory_order_release);
    cv_.notify_all();
}

TileSelectionShadowRunner* TileSelectionWorker::tryTakeResult() {
    assert(std::this_thread::get_id() == renderThreadId_ &&
           "tryTakeResult must run on the render thread");
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
