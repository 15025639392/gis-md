#pragma once

#include "TileSelectionShadowRunner.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace earth_engine {

class TilesetTileRegistry;

/// Dedicated selection worker thread (NOT the AsyncSystem decode pool — keeping
/// it separate avoids starving content decoding). It runs the heavy shadow-tree
/// traversal off the render thread.
///
/// Non-blocking protocol (true async, step 5). Ownership of the internal runner
/// alternates between the render thread and the worker, mediated by `busy_`
/// (atomic, acquire/release) plus the mutex-guarded job/result handoff:
///
///   render frame:
///     if (const runner = tryTakeResult()) reconcile(runner) -> live
///     if (!isBusy()) { buildShadow(live); dispatch(snapshot); }
///     else: worker still selecting -> draw the last reconciled plan (fallback)
///
/// While the worker is selecting (busy_ == true), the render thread never
/// touches the runner. buildShadow/dispatch are only called after isBusy()
/// reads false, whose acquire pairs with the worker's release of busy_ = false,
/// so the render thread's runner access happens-after the worker's writes. The
/// worker reads only the shadow (already built by buildShadow) and the VALUE
/// snapshot in the dispatched input — never live per-frame state.
class TileSelectionWorker {
public:
    TileSelectionWorker();
    ~TileSelectionWorker();

    TileSelectionWorker(const TileSelectionWorker&) = delete;
    TileSelectionWorker& operator=(const TileSelectionWorker&) = delete;

    /// True while the worker is selecting on the shadow. When false, the render
    /// thread owns the runner and may buildShadow()/dispatch() or read a result.
    bool isBusy() const { return busy_.load(std::memory_order_acquire); }

    /// Render thread, requires !isBusy(): rebuild the shadow tree from live.
    void buildShadow(const TilesetTileRegistry& liveRegistry);

    /// Render thread, requires !isBusy(): copy the per-frame value snapshot and
    /// kick the worker to select on the shadow just built.
    void dispatch(const TileSelectionShadowSelectInput& input);

    /// Render thread: if the worker has published a fresh result, consume it and
    /// return the runner (caller reconciles onto live); otherwise nullptr. The
    /// returned reference is valid until the next dispatch()/buildShadow().
    const TileSelectionShadowRunner* tryTakeResult();

private:
    void threadMain();

    TileSelectionShadowRunner runner_;
    TileSelectionShadowSelectInput job_;

    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<bool> busy_{false};
    bool jobReady_ = false;
    bool resultReady_ = false;
    bool stop_ = false;
};

} // namespace earth_engine
